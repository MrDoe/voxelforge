#!/usr/bin/env python3
# gen_textures.py - bake the optional material photo-textures into the asset
# dir (gitignored like every other asset).
#
# The renderer samples these as ALBEDO (R8G8B8A8_UNORM, byte/255 -> linear
# albedo), so the colours are authored to match what the *palette plus the
# per-material detail accents* produced before textures existed: each texture
# keeps the material's identity colour (grass stays grass-green, rock stays
# grey) and bakes in the character the shader accents used to add, then adds
# real multiscale structure on top. That is the "same look, more detail"
# contract - verify with tests/texture_check.py and a VF_TEXTURES=0 A/B.
#
# Every field is periodic by construction (wrapped value noise, integer-
# frequency sines), so the triplanar world-space projection tiles seamlessly.
#
# Usage: python3 tools/gen_textures.py [--out DIR] [--size N] [--seed N]
#        ninja -C build textures

import argparse
import os

import numpy as np
from PIL import Image

# Material targets: the effective mean colour of each material as the old
# palette + accents rendered it (see shaders/common_base.glsl detailAlbedo).
# Values are sRGB byte triples because that is exactly what the sampler reads.
TARGETS = {
    "grass_dark": (25, 97, 21),     # mat 0  (palette 18,133,15 after accents)
    "grass_light": (45, 129, 32),   # mat 1  (palette 41,173,26 after accents)
    "soil": (155, 90, 35),          # mat 2
    "sand": (210, 180, 95),         # mat 3
    "rock": (115, 103, 90),         # mat 4
    "light_rock": (156, 152, 140),  # mat 5
    "wood": (120, 62, 20),          # mat 6
    "shingles": (94, 59, 26),       # mat 7
    "foliage": (13, 76, 12),        # mat 8
    "snow": (223, 230, 240),        # mat 16
    "bark": (92, 56, 26),           # mat 17 (palette 0.36,0.22,0.10)
    "moss": (51, 97, 31),           # mat 18 (palette 0.20,0.38,0.12)
    "thatch": (158, 128, 51),       # mat 19 (palette 0.62,0.50,0.20)
    "plaster": (204, 194, 179),     # mat 20 (palette 0.80,0.76,0.70)
    "lava_rock": (52, 42, 38),      # mat 9: cooled basalt (the emissive term
                                    #       carries the orange glow)
}


def vnoise(n, cx, cy, rng):
    """Periodic bilinear-smoothstep value noise on an n x n grid.

    The lattice wraps, so the result tiles exactly in both axes (required by
    the repeat sampler under triplanar projection)."""
    g = rng.random((cy, cx))
    xs = (np.arange(n) * (cx / n))
    ys = (np.arange(n) * (cy / n))
    x0 = np.floor(xs).astype(int) % cx
    y0 = np.floor(ys).astype(int) % cy
    fx = xs - np.floor(xs)
    fy = ys - np.floor(ys)
    fx = (fx * fx * (3.0 - 2.0 * fx))[None, :]
    fy = (fy * fy * (3.0 - 2.0 * fy))[:, None]
    x1 = (x0 + 1) % cx
    y1 = (y0 + 1) % cy
    v00 = g[np.ix_(y0, x0)]
    v01 = g[np.ix_(y0, x1)]
    v10 = g[np.ix_(y1, x0)]
    v11 = g[np.ix_(y1, x1)]
    top = v00 * (1.0 - fx) + v01 * fx
    bot = v10 * (1.0 - fx) + v11 * fx
    return top * (1.0 - fy) + bot * fy


def fbm(n, rng, cells=4, octaves=5, gain=0.5, aniso=(1.0, 1.0), ridge=False):
    acc = np.zeros((n, n))
    amp, norm = 1.0, 0.0
    for o in range(octaves):
        cx = max(2, int(round(cells * aniso[0] * (2 ** o))))
        cy = max(2, int(round(cells * aniso[1] * (2 ** o))))
        v = vnoise(n, cx, cy, rng)
        if ridge:
            v = 1.0 - np.abs(2.0 * v - 1.0)
        acc += amp * v
        norm += amp
        amp *= gain
    return acc / norm


def smoothstep(a, b, x):
    # signed guard: descending ramps (b < a) are legal - they build "inside"
    # masks - so the epsilon must not flip or clamp the denominator sign
    d = b - a
    if abs(d) < 1e-9:
        d = 1e-9
    t = np.clip((x - a) / d, 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def norm_mean(L):
    return L / max(float(L.mean()), 1e-6)


def colorize(L, target, mods=None):
    """L: relative luminance (mean 1). mods: list of (dr,dg,db) arrays
    multiplied per channel - used for hue variation (e.g. dry grass yellowing)."""
    L = norm_mean(L)
    t = np.asarray(target, dtype=np.float64) / 255.0
    img = np.repeat(L[:, :, None], 3, axis=2) * t[None, None, :]
    if mods:
        for dr, dg, db in mods:
            img[:, :, 0] *= dr
            img[:, :, 1] *= dg
            img[:, :, 2] *= db
    return np.clip(img, 0.0, 1.0)


# ---- per-material generators -------------------------------------------------

def gen_grass(n, rng, light):
    clumps = fbm(n, rng, cells=5, octaves=4)
    fine = fbm(n, rng, cells=42, octaves=2, aniso=(1.7, 0.9))
    blades = fbm(n, rng, cells=24, octaves=2, aniso=(2.6, 0.8))
    L = (0.70 + 0.62 * clumps) * (0.86 + 0.28 * fine) * (0.93 + 0.16 * blades)
    if light:
        L *= 1.06
    dry = smoothstep(0.42, 0.86, fbm(n, rng, cells=3, octaves=3))
    return colorize(L, TARGETS["grass_light" if light else "grass_dark"],
                    mods=[(1.0 + 0.30 * dry, 1.0 - 0.03 * dry, 1.0 - 0.45 * dry)])


def gen_soil(n, rng):
    clods = fbm(n, rng, cells=7, octaves=4)
    grain = fbm(n, rng, cells=64, octaves=2)
    L = (0.62 + 0.78 * clods) * (0.90 + 0.20 * grain)
    damp = smoothstep(0.45, 0.85, fbm(n, rng, cells=2, octaves=2))
    L *= 1.0 - 0.28 * damp
    # sparse pebbles: bright mineral flecks
    peb = vnoise(n, 78, 78, rng)
    mask = smoothstep(0.90, 0.99, peb)
    img = colorize(L, TARGETS["soil"])
    img = img * (1.0 - mask[:, :, None]) + mask[:, :, None] * np.array(
        [0.62, 0.60, 0.56])[None, None, :] * norm_mean(L)[:, :, None]
    return np.clip(img, 0.0, 1.0)


def gen_sand(n, rng):
    warp = fbm(n, rng, cells=4, octaves=3)
    phase = np.arange(n)[None, :] * 0.0 + np.arange(n)[:, None] * 5.0 + 3.0 * warp
    ripples = 0.5 + 0.5 * np.sin(2.0 * np.pi * phase)
    grain = fbm(n, rng, cells=96, octaves=2)
    L = (0.82 + 0.30 * ripples) * (0.90 + 0.20 * grain)
    # scattered darker mineral grains
    dark = smoothstep(0.88, 0.98, vnoise(n, 110, 110, rng))
    L *= 1.0 - 0.30 * dark
    return colorize(L, TARGETS["sand"])


def _rock(n, rng, key):
    strata = fbm(n, rng, cells=3, octaves=3)
    phase = np.arange(n)[:, None] * 4.0 + 2.4 * strata
    bands = 0.5 + 0.5 * np.sin(2.0 * np.pi * phase)
    mottle = fbm(n, rng, cells=12, octaves=4)
    cracks = smoothstep(0.78, 0.96, fbm(n, rng, cells=6, octaves=3, ridge=True))
    L = (0.86 + 0.24 * bands) * (0.80 + 0.40 * mottle) * (1.0 - 0.42 * cracks)
    # weathered flakes: fine angular chips
    chips = smoothstep(0.80, 0.97, vnoise(n, 70, 70, rng))
    L *= 1.0 + 0.16 * chips
    return colorize(L, TARGETS[key])


def gen_rock(n, rng):
    return _rock(n, rng, "rock")


def gen_light_rock(n, rng):
    return _rock(n, rng, "light_rock")


def gen_wood(n, rng):
    # vertical grain: phase drifts along the grain axis (world Y maps here)
    drift = fbm(n, rng, cells=3, octaves=3, aniso=(3.0, 0.6))
    u = np.arange(n)[None, :] / n
    phase = u * 11.0 + 2.0 * drift
    rings = 0.5 + 0.5 * np.sin(2.0 * np.pi * phase)
    rings = rings ** 0.7
    late = smoothstep(0.72, 0.97, rings)          # dark latewood lines
    fibre = fbm(n, rng, cells=90, octaves=2, aniso=(4.0, 0.7))
    L = 1.04 - 0.34 * late + 0.10 * (fibre - 0.5)
    # a couple of knots: dark elliptical blobs with a swirl
    img = colorize(L, TARGETS["wood"])
    yy, xx = np.mgrid[0:n, 0:n] / float(n)
    for kx, ky, kr in ((0.28, 0.22, 0.055), (0.72, 0.68, 0.040)):
        dx = np.minimum(np.abs(xx - kx), 1.0 - np.abs(xx - kx))
        dy = np.minimum(np.abs(yy - ky), 1.0 - np.abs(yy - ky))
        r = np.sqrt(dx * dx + dy * dy)
        knot = smoothstep(kr, kr * 0.35, r)
        ring = smoothstep(kr * 2.6, kr * 1.1, r) * (1.0 - knot)
        img *= 1.0 - 0.45 * knot[:, :, None] - 0.10 * ring[:, :, None]
    return np.clip(img, 0.0, 1.0)


def gen_shingles(n, rng):
    # staggered courses of individual shingles (not a sine grating): per-tile
    # tone, half-column offset per row, dark seams under each course edge and
    # between neighbours
    rows, cols = 8, 6
    u = np.arange(n)[None, :] / n
    v = np.arange(n)[:, None] / n
    warp = (fbm(n, rng, cells=4, octaves=2) - 0.5) * 0.12
    rv = v * rows + warp
    r = np.floor(rv).astype(int) % rows
    fr = rv - np.floor(rv)
    cu = u * cols + 0.5 * (r % 2) + 0.5 * warp
    c = np.floor(cu).astype(int) % cols
    fc = cu - np.floor(cu)
    tone = rng.random((rows, cols))[r, c]
    # shingle body: dark gap along the top of the course and both side seams,
    # slightly shaded toward the exposed (bottom) edge
    body = (smoothstep(0.0, 0.10, fc) * smoothstep(1.0, 0.90, fc) *
            smoothstep(0.0, 0.16, fr) * (0.86 + 0.20 * fr))
    L = (0.66 + 0.30 * tone) * (0.52 + 0.62 * body)
    fine = fbm(n, rng, cells=70, octaves=2, aniso=(1.5, 1.0))
    L *= 0.90 + 0.18 * fine
    moss = smoothstep(0.55, 0.92, fbm(n, rng, cells=4, octaves=3))
    return colorize(L, TARGETS["shingles"],
                   mods=[(1.0 - 0.10 * moss, 1.0 + 0.16 * moss, 1.0 - 0.18 * moss)])


def gen_foliage(n, rng):
    clumps = fbm(n, rng, cells=9, octaves=3, ridge=True)
    leaves = fbm(n, rng, cells=34, octaves=3, ridge=True)
    L = (0.45 + 1.05 * clumps) * (0.78 + 0.44 * leaves)
    return colorize(L, TARGETS["foliage"])


def gen_snow(n, rng):
    drift = fbm(n, rng, cells=3, octaves=4)
    grain = fbm(n, rng, cells=70, octaves=2)
    L = (0.96 + 0.09 * drift) * (0.985 + 0.030 * grain)
    sparkle = smoothstep(0.955, 0.999, vnoise(n, 64, 64, rng))
    L = L * (1.0 + 0.35 * sparkle)
    return colorize(L, TARGETS["snow"])


def gen_bark(n, rng):
    # trunk fibres: long vertical streaks whose phase drifts across the
    # surface, crossed by dark annual rings (the same axes wood uses, rougher)
    drift = fbm(n, rng, cells=4, octaves=3, aniso=(3.0, 0.6))
    v = np.arange(n)[:, None] / n
    phase = v * 9.0 + 2.4 * drift
    rings = 0.5 + 0.5 * np.sin(2.0 * np.pi * phase)
    fibre = fbm(n, rng, cells=110, octaves=2, aniso=(4.0, 0.7))
    L = (0.92 - 0.30 * rings) * (0.86 + 0.24 * fibre)
    # flaking plates: patchy scales lift off the trunk
    plates = smoothstep(0.55, 0.92, fbm(n, rng, cells=14, octaves=3))
    L *= 1.0 - 0.24 * plates
    # moss creeps in the fissures (north-side damp)
    moss = smoothstep(0.60, 0.95, fbm(n, rng, cells=5, octaves=2))
    return colorize(L, TARGETS["bark"],
                    mods=[(1.0 - 0.25 * moss, 1.0 + 0.35 * moss, 1.0 - 0.30 * moss)])


def gen_moss(n, rng):
    clumps = fbm(n, rng, cells=9, octaves=4, ridge=True)
    mats = fbm(n, rng, cells=30, octaves=3)
    L = (0.55 + 0.80 * clumps) * (0.85 + 0.25 * mats)
    damp = smoothstep(0.4, 0.8, fbm(n, rng, cells=3, octaves=2))
    L *= 1.0 - 0.18 * damp
    # dry tips bleach where the clump stands proud
    dry = smoothstep(0.7, 0.95, clumps)
    return colorize(L, TARGETS["moss"],
                    mods=[(1.0 + 0.25 * dry, 1.0 + 0.05 * dry, 1.0 - 0.20 * dry)])


def gen_thatch(n, rng):
    # bundled reeds: long vertical fibres grouped into sheaves, sewn into
    # horizontal courses with a shadow under each binding
    rows = 7
    u = np.arange(n)[None, :] / n
    v = np.arange(n)[:, None] / n
    warp = (fbm(n, rng, cells=4, octaves=2) - 0.5) * 0.10
    rv = v * rows + warp
    fr = rv - np.floor(rv)
    sheaf = 0.5 + 0.5 * np.sin(
        2.0 * np.pi * (u * 26.0 + 2.0 * fbm(n, rng, cells=6, octaves=2,
                                            aniso=(0.4, 3.0))))
    reed = fbm(n, rng, cells=120, octaves=2, aniso=(0.4, 6.0))
    L = (0.72 + 0.34 * sheaf) * (0.86 + 0.22 * reed)
    L *= 0.78 + 0.32 * smoothstep(0.0, 0.18, fr)
    L *= 0.90 + 0.18 * fbm(n, rng, cells=5, octaves=2)
    return colorize(L, TARGETS["thatch"])


def gen_plaster(n, rng):
    # white-wash: soft lime mottling, hairline shrinkage cracks, faint drip
    # streaks below the roof line
    mottle = fbm(n, rng, cells=6, octaves=4)
    fine = fbm(n, rng, cells=50, octaves=2)
    L = (0.94 + 0.10 * mottle) * (0.96 + 0.06 * fine)
    cracks = smoothstep(0.86, 0.97, fbm(n, rng, cells=8, octaves=3, ridge=True))
    L *= 1.0 - 0.35 * cracks
    streak = smoothstep(0.5, 0.9, fbm(n, rng, cells=3, octaves=2, aniso=(0.4, 3.0)))
    L *= 1.0 - 0.10 * streak
    return colorize(L, TARGETS["plaster"])


def gen_lava(n, rng):
    # cooled basalt: dark rough crust pocked with burst bubbles, lit from
    # below by molten fissures (the material's emissive term adds the glow)
    rock = fbm(n, rng, cells=14, octaves=4)
    pores = smoothstep(0.70, 0.95, fbm(n, rng, cells=40, octaves=2, ridge=True))
    L = (0.34 + 0.30 * rock) * (1.0 - 0.40 * pores)
    veins = smoothstep(0.55, 0.75, fbm(n, rng, cells=4, octaves=3, ridge=True))
    glow = smoothstep(0.40, 0.90, fbm(n, rng, cells=22, octaves=2))
    img = colorize(L, TARGETS["lava_rock"])
    hot = (veins * (0.5 + 0.8 * glow))[:, :, None]
    img = img * (1.0 - hot) + hot * np.array([1.0, 0.45, 0.08])[None, None, :]
    return np.clip(img, 0.0, 1.0)


GENERATORS = {
    "grass_dark": lambda n, r: gen_grass(n, r, False),
    "grass_light": lambda n, r: gen_grass(n, r, True),
    "soil": gen_soil,
    "sand": gen_sand,
    "rock": gen_rock,
    "light_rock": gen_light_rock,
    "wood": gen_wood,
    "shingles": gen_shingles,
    "foliage": gen_foliage,
    "snow": gen_snow,
    "bark": gen_bark,
    "moss": gen_moss,
    "thatch": gen_thatch,
    "plaster": gen_plaster,
    "lava": gen_lava,
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "assets", "textures"))
    ap.add_argument("--prefix", default="proc_",
                    help="filename prefix (the fetched set uses none, so the "
                         "offline textures stay selectable side by side)")
    ap.add_argument("--size", type=int, default=256)
    ap.add_argument("--seed", type=int, default=20260919)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    for i, (name, fn) in enumerate(sorted(GENERATORS.items())):
        rng = np.random.default_rng(args.seed + i * 977)
        img = fn(args.size, rng)
        # the atlas box-downsamples to 128^2; give it a clean 2x source
        arr = (np.round(np.clip(img, 0.0, 1.0) * 255.0)).astype(np.uint8)
        path = os.path.join(args.out, args.prefix + name + ".png")
        Image.fromarray(arr, "RGB").save(path)
        print("texture %-12s %dx%d -> %s (mean %s)"
              % (name, args.size, args.size, path,
                 tuple(int(v) for v in arr.reshape(-1, 3).mean(axis=0).round())))


if __name__ == "__main__":
    main()
