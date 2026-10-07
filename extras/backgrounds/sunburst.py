#!/usr/bin/env python3
"""GemWM's sunburst wallpaper: rays in warm 70s/80s colours rising from a
sun below the bottom edge, fading to cream outwards (shades for GemWM's
dithering to work with). Usage: sunburst.py WIDTH HEIGHT OUT"""
import math, sys
from PIL import Image, ImageDraw, ImageFilter

W, H, out = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
SS = 2                                       # drawn twice the size, then shrunk
w, h = W * SS, H * SS
cx, cy = w * 0.5, h * 1.12                   # the sun, below the bottom edge
RAYS = [(232, 112, 42), (242, 177, 52), (200, 64, 42),
        (42, 122, 106), (122, 74, 42), (236, 150, 60)]
N = 28
CREAM = (246, 226, 180)

rays = Image.new('RGB', (w, h))
d = ImageDraw.Draw(rays)
R = math.hypot(w, h) * 2
for i in range(N):
    d.pieslice([cx - R, cy - R, cx + R, cy + R],
               -180 + i * 360 / N, -180 + (i + 1) * 360 / N, fill=RAYS[i % len(RAYS)])

# The fade to cream: worked out on a small grid, then enlarged smoothly.
gw, gh = 480, 270
mask = Image.new('L', (gw, gh))
m = mask.load()
for y in range(gh):
    for x in range(gw):
        dist = math.hypot((x + 0.5) / gw - 0.5, ((y + 0.5) / gh - 1.12) * h / w) / math.hypot(0.5, 1.12 * h / w)
        t = min(1, max(0, (dist - 0.35) / 0.9))
        m[x, y] = int(255 * t * 0.55)
mask = mask.resize((w, h), Image.BICUBIC)
img = Image.composite(Image.new('RGB', (w, h), CREAM), rays, mask)

d = ImageDraw.Draw(img)
s = w / 1280                                  # ring sizes were made for 1280 wide
for rad, col in [(330, (176, 52, 36)), (290, (214, 88, 40)), (250, (236, 130, 48)),
                 (210, (246, 176, 70)), (170, (250, 214, 120))]:
    r = rad * s
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=col)

img = img.resize((W, H), Image.LANCZOS)
if out.endswith('.jpg'):
    img.save(out, quality=94, optimize=True, progressive=True)
else:
    img.save(out, optimize=True)
