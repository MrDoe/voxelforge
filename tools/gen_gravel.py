#!/usr/bin/env python3
"""Generate a tileable gravel texture: assets/textures/gravel.png.

Deterministic (fixed RNG seed). Packed grey pebbles with a lit top-left and a
shadowed lower rim, plus fine grain. The atlas binds it to a spare material
slot (mat 15) and the forest path samples it through the per-cell texture
override, so no material palette entry is disturbed.
"""
import random

from PIL import Image, ImageDraw

S = 512
rng = random.Random(20261004)
img = Image.new("RGB", (S, S), (58, 55, 52))
dr = ImageDraw.Draw(img)


def pebble(cx, cy, rx, ry, base):
    # lower rim shadow, body, top-left highlight (5 stacked ellipses)
    for ox, oy, shade in ((2.2, 2.4, -30), (1.1, 1.2, -18), (0, 0, 0),
                          (-1.1, -1.2, 20), (-2.0, -2.2, 36)):
        col = tuple(max(0, min(255, int(c + shade))) for c in base)
        dr.ellipse([cx - rx + ox, cy - ry + oy, cx + rx + ox, cy + ry + oy],
                   fill=col)


for i in range(2600):
    cx = rng.uniform(0, S)
    cy = rng.uniform(0, S)
    rx = rng.uniform(3.5, 11.0)
    ry = rx * rng.uniform(0.72, 1.28)
    g = rng.randint(88, 192)
    warm = rng.randint(-8, 12)
    base = (g + warm, g, g - 6 - warm // 2)
    for dx in (-S, 0, S):
        for dy in (-S, 0, S):
            if (-rx - 4 < cx + dx < S + rx + 4
                    and -ry - 4 < cy + dy < S + ry + 4):
                pebble(cx + dx, cy + dy, rx, ry, base)

px = img.load()
for y in range(S):
    for x in range(S):
        n = rng.randint(-9, 9)
        r, g, b = px[x, y]
        px[x, y] = (max(0, min(255, r + n)), max(0, min(255, g + n)),
                    max(0, min(255, b + n)))

img.save("assets/textures/gravel.png")
print("wrote assets/textures/gravel.png (512x512, tileable)")
