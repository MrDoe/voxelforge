#!/usr/bin/env python3
"""Build a DENSE meadow photo card from the CC0 Wild-Grass-01 blades.

The source atlas (T_Wild_Grass_01 basecolor + opacity, CC0 from 3DTexel) is a
sparse clump of a few blades. A single such clump per tuft reads as a handful
of isolated straws, not the continuous tall meadow the scene wants. This tool
composites many independently rotated, scaled, flipped and colour-shifted
copies of those blades into one 512x512 RGBA card, so each tuft is a dense
clump of dozens of photographic blades with natural green/straw variation.

Only the CC0 source is read; the output is written into the active scenery's
shared card library (assets/textures/cards/card_23_grass.png). Re-running the
packer (pack_foliage_cards.py) will overwrite it, so run THIS afterwards.

Usage: gen_dense_grass_card.py [--src maps-dir] [--out card.png] [--count N]
"""
import argparse
import math
import os
import random

from PIL import Image, ImageEnhance, ImageFilter

SRC_BASE = "T_Wild_Grass_01_basecolor_Jpeg.webp"
SRC_ALPHA = "T_Wild_Grass_01_Opacity_Jpeg.webp"
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_blades(src):
    rgb = Image.open(os.path.join(src, SRC_BASE)).convert("RGB")
    alpha = Image.open(os.path.join(src, SRC_ALPHA)).convert("L")
    # A photographic blade has soft edges; the atlas opacity is a near-binary
    # cutout, so a sub-pixel blur gives the slivers translucent rims instead
    # of hard aliased edges (the reference's blades are wispy, not stamped).
    alpha = alpha.filter(ImageFilter.GaussianBlur(0.2))
    rgba = Image.merge("RGBA", (*rgb.split(), alpha))
    # crop to the opaque bounding box so clump scaling is independent of the
    # source atlas padding
    bbox = alpha.getbbox()
    return rgba.crop(bbox) if bbox else rgba


def tint_blade(im, rng):
    """Shift a CC0 green blade toward straw / gold / deep green."""
    r, g, b, a = im.split()
    # random gain per channel: g dominant, r for dry straw, b kept low
    gr = rng.uniform(0.82, 1.18)
    rr = rng.uniform(0.85, 1.55)
    br = rng.uniform(0.55, 1.10)
    r = r.point(lambda v: min(255, int(v * rr)))
    g = g.point(lambda v: min(255, int(v * gr)))
    b = b.point(lambda v: min(255, int(v * br)))
    out = Image.merge("RGBA", (r, g, b, a))
    return ImageEnhance.Brightness(out).enhance(rng.uniform(0.80, 1.22))


def paste_wrap(canvas, tile, x, y, alpha=1.0):
    """Alpha-composite a tile at (x,y), wrapping horizontally for seamless U."""
    w, h = canvas.size
    tw, th = tile.size
    if alpha != 1.0:
        tile = tile.copy()
        a = tile.getchannel("A").point(lambda v: int(v * alpha))
        tile.putalpha(a)
    for ox in range(-1, 2):
        px = int(x) + ox * w
        if px + tw <= 0 or px >= w:
            continue
        canvas.alpha_composite(tile, (px, int(y)))


def build(src, out, size, count, seed):
    rng = random.Random(seed)
    blades = load_blades(src)
    # NO synthetic underlay. The ground already carries a photographic grass
    # texture, so transparent gaps between the blades reveal REAL grass ground
    # rather than brown soil. A painted green base reads as a flat carpet.
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))

    # background layer: short, darker clumps fill the lower third (the roots
    # and dense undergrowth), then taller bright blades on top.
    layers = [
        (int(count * 0.55), 0.42, 0.78, 0.62, 0.86, 0.30, 0.62, 0.50),
        (int(count * 0.30), 0.78, 1.12, 0.72, 1.02, 0.10, 0.42, 0.80),
        (count - int(count * 0.55) - int(count * 0.30), 1.05, 1.45,
         0.78, 1.10, 0.02, 0.22, 1.00),
    ]
    for n, s0, s1, base0, base1, y0, y1, alpha in layers:
        for _ in range(max(0, n)):
            scale = rng.uniform(s0, s1)
            bw = max(1, int(blades.width * scale))
            bh = max(1, int(blades.height * scale))
            tile = blades.resize((bw, bh), Image.LANCZOS)
            if rng.random() < 0.5:
                tile = tile.transpose(Image.FLIP_LEFT_RIGHT)
            ang = rng.uniform(-16.0, 16.0)
            if abs(ang) > 0.5:
                tile = tile.rotate(ang, resample=Image.BICUBIC, expand=True,
                                   fillcolor=(0, 0, 0, 0))
            tile = tint_blade(tile, rng)
            # base near the bottom; taller layer reaches higher up the card
            by = int(size * rng.uniform(y0, y1))
            y = size - tile.height - by
            x = rng.uniform(-tile.width * 0.2, size - tile.width * 0.8)
            paste_wrap(canvas, tile, x, y, alpha)

    # Dissolve the card silhouette: fade alpha at both side edges and the top
    # (tips) so overlapping cards never reveal a rectangular boundary. The
    # base stays opaque because it sits on the ground under other cards.
    px = canvas.load()
    for y in range(size):
        fy = min(1.0, y / (size * 0.16))
        for x in range(size):
            fx = min(1.0, min(x, size - 1 - x) / (size * 0.10))
            f = min(fx, fy)
            if f < 1.0:
                r, g, b, a = px[x, y]
                px[x, y] = (r, g, b, int(a * f))

    canvas.save(out)
    opaque = sum(1 for p in canvas.getchannel("A").getdata() if p > 24)
    print("wrote %s (%d/%d px opaque, %.1f%%)" %
          (out, opaque, size * size, 100.0 * opaque / (size * size)))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--src", default="/home/christoph/code/plantcards-texel/maps")
    ap.add_argument("--out", default=os.path.join(
        REPO, "assets", "textures", "cards", "card_23_grass.png"))
    ap.add_argument("--size", type=int, default=512)
    ap.add_argument("--count", type=int, default=26)
    ap.add_argument("--seed", type=int, default=1337)
    a = ap.parse_args()
    build(a.src, a.out, a.size, a.count, a.seed)


if __name__ == "__main__":
    main()
