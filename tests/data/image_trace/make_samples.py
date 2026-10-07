"""Generate the sample images for the image trace hand test (Add part > Image...).

All images are drawn here from simple shapes, no third party artwork. Run from this folder:
    python make_samples.py
"""
import math

from PIL import Image, ImageDraw


def star(cx, cy, r_out, r_in, points=5, rotation=-90.):
    pts = []
    for i in range(points * 2):
        r = r_out if i % 2 == 0 else r_in
        a = math.radians(rotation + i * 180. / points)
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def heart(cx, cy, size, steps=200):
    pts = []
    for i in range(steps):
        t = 2 * math.pi * i / steps
        x = 16 * math.sin(t) ** 3
        y = 13 * math.cos(t) - 5 * math.cos(2 * t) - 2 * math.cos(3 * t) - math.cos(4 * t)
        pts.append((cx + x * size / 32., cy - y * size / 32.))
    return pts


def supersampled(w, h, mode, background, draw_fn, factor=4):
    big = Image.new(mode, (w * factor, h * factor), background)
    draw_fn(ImageDraw.Draw(big), factor)
    return big.resize((w, h), Image.LANCZOS)


# 1. Badge with transparent background: navy disc, yellow ring, white star (3 colours + transparency)
def badge(d, f):
    d.ellipse([16 * f, 16 * f, 240 * f, 240 * f], fill=(250, 200, 20, 255))
    d.ellipse([34 * f, 34 * f, 222 * f, 222 * f], fill=(20, 40, 110, 255))
    d.polygon([(x * f, y * f) for x, y in star(128, 132, 80, 34)], fill=(255, 255, 255, 255))


supersampled(256, 256, 'RGBA', (0, 0, 0, 0), badge).save('badge_transparent.png', optimize=True)


# 2. Opaque sign on white: black frame, red heart, green triangle (background is left out by default)
def sign(d, f):
    d.rounded_rectangle([10 * f, 10 * f, 310 * f, 190 * f], radius=24 * f, outline=(0, 0, 0), width=10 * f)
    d.polygon([(x * f, y * f) for x, y in heart(105, 100, 110)], fill=(210, 30, 40))
    d.polygon([(190 * f, 150 * f), (290 * f, 150 * f), (240 * f, 50 * f)], fill=(30, 140, 60))


supersampled(320, 200, 'RGB', (255, 255, 255), sign).save('sign_on_white.png', optimize=True)
# the same as JPEG (compression artifacts, no transparency), also used by test_image_trace.cpp
supersampled(320, 200, 'RGB', (255, 255, 255), sign).save('sign_on_white.jpg', quality=85)


# 3. Gray radial gradient with a dark ring, for posterise / stepped relief
w = h = 200
img = Image.new('L', (w, h), 255)
px = img.load()
for y in range(h):
    for x in range(w):
        r = math.hypot(x - w / 2, y - h / 2) / (w / 2)
        v = 40 + 215 * min(r, 1.)
        if 0.55 < r < 0.62:
            v = 20
        px[x, y] = int(v)
img.save('radial_gradient.png', optimize=True)
