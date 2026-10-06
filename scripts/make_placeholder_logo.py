"""Generate the UltraOne placeholder mark: a geometric "U1" monogram.

The mark is one shape family described once, below, and emitted twice - as SVG (for the
splash and the About dialog, which the app renders through NanoSVG, so paths only, no
<text>) and as PNG/ICO/ICNS rasters drawn with the same numbers in Pillow. Nothing here
is traced from anyone else's logo.

Every file keeps the name it already had in resources/images, so replacing this with a
designed logo later is a matter of dropping files in over these.

Usage:  python scripts/make_placeholder_logo.py resources/images
"""
import io, os, struct, sys
from PIL import Image, ImageDraw

OUT = sys.argv[1] if len(sys.argv) > 1 else "."
os.makedirs(OUT, exist_ok=True)

BG    = (28, 29, 33, 255)     # #1C1D21 - the ground the mark sits on
WHITE = (255, 255, 255, 255)  # the U
ONE   = (10, 158, 80, 255)    # #0A9E50 - the accent already used by the hub pages

# --- the geometry, in fractions of the canvas side -------------------------------------
CORNER = 0.200   # rounded-square radius
DX     = 0.0225  # the glyph's own centre is left of the canvas centre; nudge it back
T, B   = 0.300, 0.720          # cap height and baseline
W      = 0.095                 # stroke weight
UL, UR = 0.215 + DX, 0.545 + DX   # the U's outer left and right
BX0    = 0.645 + DX            # the 1's stem
BX1    = 0.740 + DX
FLAGX  = 0.560 + DX            # where the 1's flag reaches back to
FLAGY  = 0.405                 # and how far down the stem it starts


def svg_body(scale, ox=0.0, oy=0.0, glyph_scale=1.0, one_colour="#0A9E50", u_colour="#FFFFFF"):
    """The two glyph paths, in user units of a `scale`-sided square."""
    def x(v):
        return (0.5 + (v - 0.5) * glyph_scale) * scale + ox
    def y(v):
        return (0.5 + (v - 0.5) * glyph_scale) * scale + oy
    def d(v):
        return v * glyph_scale * scale

    ro = (UR - UL) / 2.0
    ri = ro - W
    cy = B - ro
    u = ("M%.2f %.2f V%.2f A%.2f %.2f 0 0 0 %.2f %.2f V%.2f H%.2f V%.2f "
         "A%.2f %.2f 0 0 1 %.2f %.2f V%.2f Z") % (
        x(UL), y(T), y(cy), d(ro), d(ro), x(UR), y(cy), y(T), x(UR - W), y(cy),
        d(ri), d(ri), x(UL + W), y(cy), y(T))
    # the 1: a stem plus a flag, one closed outline
    one = ("M%.2f %.2f L%.2f %.2f L%.2f %.2f L%.2f %.2f L%.2f %.2f L%.2f %.2f Z") % (
        x(FLAGX), y((T + FLAGY) / 2.0 + 0.012),
        x(BX0), y(T),
        x(BX1), y(T),
        x(BX1), y(B),
        x(BX0), y(B),
        x(BX0), y(FLAGY + 0.020))
    return ('<path d="%s" fill="%s"/>\n<path d="%s" fill="%s"/>' % (u, u_colour, one, one_colour))


def svg(side, ground=True, glyph_scale=1.0, u_colour="#FFFFFF", one_colour="#0A9E50",
        vw=None, vh=None, ox=0.0, oy=0.0):
    w = vw if vw is not None else side
    h = vh if vh is not None else side
    parts = ['<svg xmlns="http://www.w3.org/2000/svg" width="%g" height="%g" viewBox="0 0 %g %g">'
             % (w, h, w, h)]
    if ground:
        parts.append('<rect x="%g" y="%g" width="%g" height="%g" rx="%g" fill="#1C1D21"/>'
                     % (ox, oy, side, side, CORNER * side))
    parts.append(svg_body(side, ox, oy, glyph_scale, one_colour, u_colour))
    parts.append("</svg>")
    return "\n".join(parts) + "\n"


def raster(side, ground=True, glyph_scale=1.0, ss=4, bg=BG, u=WHITE, one=ONE):
    """Same numbers, drawn 4x and downsampled so the curves are clean at 16 px."""
    S = side * ss
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    dr = ImageDraw.Draw(im)

    def x(v):
        return (0.5 + (v - 0.5) * glyph_scale) * S
    def y(v):
        return (0.5 + (v - 0.5) * glyph_scale) * S

    if ground:
        dr.rounded_rectangle([0, 0, S - 1, S - 1], radius=CORNER * S, fill=bg)

    ro = (UR - UL) / 2.0
    # the U: an outer capsule-bottom minus an inner one
    dr.rounded_rectangle([x(UL), y(T), x(UR), y(B)], radius=ro * glyph_scale * S,
                         fill=u, corners=(False, False, True, True))
    inner_r = max(0.0, (ro - W) * glyph_scale * S)
    dr.rounded_rectangle([x(UL + W), y(T) - S, x(UR - W), y(B - W)], radius=inner_r,
                         fill=(0, 0, 0, 0) if not ground else bg,
                         corners=(False, False, True, True))
    if not ground:
        # punching a hole needs a real erase, not a paint
        hole = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        ImageDraw.Draw(hole).rounded_rectangle(
            [x(UL + W), y(T) - S, x(UR - W), y(B - W)], radius=inner_r, fill=(0, 0, 0, 255),
            corners=(False, False, True, True))
        im.putalpha(Image.composite(Image.new("L", (S, S), 0), im.getchannel("A"),
                                    hole.getchannel("A")))
        dr = ImageDraw.Draw(im)

    # the 1: stem plus flag
    dr.rectangle([x(BX0), y(T), x(BX1), y(B)], fill=one)
    dr.polygon([(x(BX0), y(T)), (x(BX0), y(FLAGY + 0.020)),
                (x(FLAGX), y((T + FLAGY) / 2.0 + 0.012))], fill=one)
    return im.resize((side, side), Image.LANCZOS)


def save(name, im):
    im.save(os.path.join(OUT, name))
    print("  ", name, im.size)


def save_text(name, s):
    io.open(os.path.join(OUT, name), "w", encoding="utf-8", newline="\n").write(s)
    print("  ", name, "(svg)")


# ---- the icon family --------------------------------------------------------------
master = raster(1024)
def sized(n):
    return master.resize((n, n), Image.LANCZOS)

for name, n in [("Snapmaker_Orca_32px.png", 32), ("Snapmaker_Orca_64.png", 64),
                ("Snapmaker_Orca_128px.png", 128), ("Snapmaker_Orca-mac_128px.png", 128),
                ("Snapmaker_Orca_154.png", 154), ("Snapmaker_Orca_154_title.png", 154),
                ("Snapmaker_Orca_192px.png", 192), ("Snapmaker_Orca_512px.png", 512),
                ("Snapmaker_Orca.png", 256), ("Snapmaker_OrcaTitle.png", 256),
                ("Snapmaker_Orca_gradient.png", 256), ("Snapmaker_Orca_gradient_circle.png", 256),
                ("Snapmaker_Orca_gradient_narrow.png", 256), ("Snapmaker_Orca_gray.png", 256)]:
    save(name, sized(n))

# the Apple touch icon is opaque RGB, as it was
save("Snapmaker_Orca_180px.png", sized(180).convert("RGB"))

# the two 192 variants the fork uses: one greyed, one with no ground behind the glyph
g = sized(192).convert("LA").convert("RGBA")
save("Snapmaker_Orca_192px_grayscale.png", g)
save("Snapmaker_Orca_192px_transparent.png", raster(192, ground=False))

# maskable: full bleed ground, glyph inside the 80% safe circle
mask = Image.new("RGBA", (512, 512), BG)
mask.alpha_composite(raster(512, ground=False, glyph_scale=0.68))
save("Snapmaker_Orca_512px_maskable.png", mask)

# ---- .ico / .icns -----------------------------------------------------------------
sizes = [(16, 16), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
master.resize((256, 256), Image.LANCZOS).save(os.path.join(OUT, "EdgeSlicer.ico"), sizes=sizes)
print("   EdgeSlicer.ico", sizes)
master.resize((154, 154), Image.LANCZOS).save(os.path.join(OUT, "EdgeSlicerTitle.ico"),
                                              sizes=[(154, 154)])
print("   EdgeSlicerTitle.ico [(154, 154)]")
master.resize((256, 256), Image.LANCZOS).save(os.path.join(OUT, "EdgeSlicer-mac_256px.ico"),
                                              sizes=[(256, 256)])
print("   EdgeSlicer-mac_256px.ico [(256, 256)]")

# ICNS by hand: the container is just a header plus PNG-carrying entries, and Pillow's own
# writer is not available on every platform.
ICNS = [(b"ic11", 32), (b"ic12", 64), (b"ic07", 128), (b"ic13", 256),
        (b"ic08", 256), (b"ic14", 512), (b"ic09", 512), (b"ic10", 1024)]
entries = []
for tag, n in ICNS:
    buf = io.BytesIO()
    master.resize((n, n), Image.LANCZOS).save(buf, format="PNG")
    data = buf.getvalue()
    entries.append(tag + struct.pack(">I", len(data) + 8) + data)
blob = b"".join(entries)
with open(os.path.join(OUT, "EdgeSlicer.icns"), "wb") as f:
    f.write(b"icns" + struct.pack(">I", len(blob) + 8) + blob)
print("   EdgeSlicer.icns", [n for _, n in ICNS])

# ---- the SVGs the app renders through NanoSVG (paths only, no <text>) ---------------
save_text("splash_app_icon.svg", svg(140))
save_text("splash_logo.svg", svg(480))
save_text("splash_logo_dark.svg", svg(480))
save_text("studio_logo.svg", svg(480))
# the About dialog logo keeps its 562x238 box; the mark is centred in it, on no ground
save_text("Snapmaker_Orca_about.svg",
          svg(210, ground=True, vw=562, vh=238, ox=(562 - 210) / 2.0, oy=(238 - 210) / 2.0))
print("done")
