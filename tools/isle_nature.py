#!/usr/bin/env python3
"""Author the LighthouseIsle scenery layers (tropical v2) via vf_mcp.

Run after tools/isle_gen.cpp has baked heightmap.png + landscape.vxw +
world.json, with VF_SCENERY=LighthouseIsle and cwd = repo root:

    VF_SCENERY=LighthouseIsle python3 tools/isle_nature.py [--only LAYER]

Layers (explicit 0.1 m lattice records):

  isle_forest    DENSE tropical jungle (~60 trees): broadleaf giants,
                 pines on the cone flanks, palms ride in their own layer.
                 Solid canopies only (surfel thin rule must never fire).
  isle_palms     ~24 coconut palms on the beach flat + cottage grove.
  (an ellipsoidal cave-mesa dome stood NE in v2.0 — the user rejected it
   2026-11-19: "what is this ugly dome? remove it". The walk-in-cave
   contract that build followed is preserved in the wiki page.)
  isle_rocks     cenote rim collar + tidal-pool rim + headland boulders +
                 offshore reef rocks + scattered beach pebbles.
  isle_lighthouse  banded tower on a ROCK PLINTH standing on the natural
                 knoll (v3: no baked pad — every object brings its own
                 footing), warm-glass lantern room; the real beacon is an
                 authored manifest light.
  isle_jetty     ramped plank deck on bark piles, shore railing, end lamp.
  isle_driftwood bleached logs on the upper beach.

NO voxel small plants (2026-11-19 user directive): ground cover is the
shading-level flora (terrain mats 0/1) and the photo-card pass in
development; trees (large) and logs (wood props) stay geometry.
"""
import argparse
import json
import math
import os
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCENERY = os.path.join(ROOT, "scenery", "LighthouseIsle")

VOXEL = 0.1
WORLD = 102.4
HALF = 0.5 * WORLD
WATER = -0.9
SEED = 4041

# palette ids (src/voxel/common.hpp). MATS 9-15 ARE EMISSIVE — the lookup keys
# on the cell material, so a non-emissive surface must NEVER use one as its
# MATERIAL (a tex override pointing at slot 15 is fine and unbound-safe).
G_DARK, G_LIGHT, SOIL, SAND, ROCK, LROCK, WOOD, ROOF, FOLIAGE = 0, 1, 2, 3, 4, 5, 6, 7, 8
EMBER, LAMP, SNOW, BARK, MOSS, THATCH, PLASTER = 10, 10, 16, 17, 18, 19, 20
WHITEHOT = 10   # every intended emitter rides mat 10 (ember, slot unbound ->
                # the record's own warm RGB shows; mat 15 would sample the
                # gravel photo bound to slot 15 AND burn a light slot)
GRAVEL = 5      # light rock RESPONSE + T_GRAVEL override samples the photo
T_BARK, T_MOSS = 17, 18
T_GRAVEL = 15

# ---- layout constants mirroring tools/isle_gen.cpp (keep in sync) ----------
RX, RZ = 36.0, 28.0
HEADLAND = (27.0, -5.0)
LIGHTHOUSE_PAD = (HEADLAND, 3.6, 5.60)
HOUSE_PAD = ((-12.0, -6.0), 4.2, 2.20)
CENOTE = (14.0, 5.0)
TIDAL = (9.0, -17.0)
JETTY_X = -2.0
JETTY_END = (JETTY_X, -24.5)
PATH = [(-11.0, -7.5), (-2.0, -14.0), (1.0, -19.0)]


def _load_hm():
    p = os.path.join(SCENERY, "heightmap.png")
    a = np.asarray(Image.open(p), dtype=np.float64)
    return -8.0 + (a / 65535.0) * 32.0


_HM = _load_hm()


def terrain(x, z):
    u = min(max(x / WORLD + 0.5, 0.0), 1.0)
    v = min(max(z / WORLD + 0.5, 0.0), 1.0)
    H, W = _HM.shape
    fx, fz = u * (W - 1), v * (H - 1)
    x0, z0 = int(np.floor(fx)), int(np.floor(fz))
    x1, z1 = min(x0 + 1, W - 1), min(z0 + 1, H - 1)
    a, b = fx - x0, fz - z0
    return float((_HM[z0, x0] * (1 - a) + _HM[z0, x1] * a) * (1 - b)
                 + (_HM[z1, x0] * (1 - a) + _HM[z1, x1] * a) * b)


def slope(x, z, e=0.6):
    gx = (terrain(x + e, z) - terrain(x - e, z)) / (2 * e)
    gz = (terrain(x, z + e) - terrain(x, z - e)) / (2 * e)
    return math.hypot(gx, gz)


def seg_dist(p, a, b):
    ax, ay = a
    bx, by = b
    px, py = p
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy + 1e-9)))
    return math.hypot(px - (ax + dx * t), py - (ay + dy * t))


def path_dist(x, z):
    p = (x, z)
    return min(seg_dist(p, PATH[0], PATH[1]), seg_dist(p, PATH[1], PATH[2]))


def cell(xm, ym, zm):
    return (int(round((xm + HALF) / VOXEL)),
            int(round((ym + HALF) / VOXEL)),
            int(round((zm + HALF) / VOXEL)))


def h3(x, y, z, s=0):
    def _i(v):
        return int(v * 1009) if isinstance(v, float) else int(v)
    n = (_i(x) * 73856093) ^ (_i(y) * 19349663) ^ (_i(z) * 83492791) ^ (_i(s) * 2654435761)
    n &= 0xFFFFFFFF
    n = (n ^ (n >> 13)) * 1274126177 & 0xFFFFFFFF
    return ((n ^ (n >> 16)) & 0xFFFF) / 65536.0


class Layer:
    def __init__(self, name):
        self.name = name
        self.cells = {}

    def put(self, x, y, z, mat, rgb, refl=60, rough=200, tex=0, amp=10, s=0):
        if not (0 <= x < 1024 and 0 <= y < 1024 and 0 <= z < 1024):
            return
        d = int((h3(x, y, z, s) - 0.5) * 2 * amp)
        self.cells[(x, y, z)] = {
            "x": x, "y": y, "z": z, "mat": mat,
            "r": max(0, min(255, rgb[0] + d)),
            "g": max(0, min(255, rgb[1] + d)),
            "b": max(0, min(255, rgb[2] + d)),
            "refl": refl, "rough": rough,
            **({"tex": tex} if tex else {}),
        }

    def blob(self, cx, cy, cz, r, mat, rgb, tex=0, squash=1.0, amp=12, s=0):
        ri = int(r) + 1
        for dx in range(-ri, ri + 1):
            for dy in range(-ri, ri + 1):
                for dz in range(-ri, ri + 1):
                    if (dx * dx + dz * dz + (dy / max(squash, 0.05)) ** 2) <= r * r:
                        self.put(cx + dx, cy + dy, cz + dz, mat, rgb,
                                 tex=tex, amp=amp, s=s)

    def column(self, cx, cy, cz, h, mat, rgb, tex=0, amp=10, s=0):
        for i in range(h):
            self.put(cx, cy + i, cz, mat, rgb, tex=tex, amp=amp, s=s + i * 7)

    def write(self):
        out = list(self.cells.values())
        if not out:
            print(f"{self.name}: EMPTY — skipped")
            return
        xs = [r["x"] for r in out]; ys = [r["y"] for r in out]; zs = [r["z"] for r in out]
        print(f"{self.name}: {len(out)} voxels, bounds x[{min(xs)}..{max(xs)}] "
              f"y[{min(ys)}..{max(ys)}] z[{min(zs)}..{max(zs)}]")
        payload = {"name": self.name, "voxels": out}
        req = ('{"jsonrpc":"2.0","id":1,"method":"tools/call",'
               '"params":{"name":"write_object","arguments":'
               + json.dumps(payload, separators=(",", ":")) + "}}\n")
        env = dict(os.environ, VF_SCENERY="LighthouseIsle")
        p = subprocess.run([os.path.join(ROOT, "build", "vf_mcp")], input=req,
                           capture_output=True, text=True, timeout=1800, cwd=ROOT, env=env)
        for line in p.stdout.splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                d = json.loads(line)
                txt = d.get("result", {}).get("content", [{}])[0].get("text", "")
                print("mcp:", txt[:240])
            except Exception:
                print("raw:", line[:200])


# ------------------------------------------------------------------ forest --
KINDS = {
    "oak":     dict(th=30, tr=2, bark=(108, 76, 42), canopy=[(56, 118, 44), (72, 140, 52), (44, 100, 38)], blobs=9, spread=(2, 7), rad=(4.5, 2.0), by=(2, 8), squash=0.85),
    "birch":   dict(th=38, tr=1, bark=(226, 222, 210), canopy=[(96, 152, 60), (118, 168, 70), (80, 132, 52)], blobs=7, spread=(2, 6), rad=(3.5, 1.5), by=(2, 8), squash=0.80),
    "beech":   dict(th=40, tr=2, bark=(126, 118, 104), canopy=[(58, 112, 44), (76, 132, 52), (46, 96, 38)], blobs=10, spread=(2, 9), rad=(5.0, 2.5), by=(1, 9), squash=0.90),
    "poplar":  dict(th=46, tr=2, bark=(152, 150, 140), canopy=[(84, 128, 52), (100, 142, 60), (70, 112, 44)], blobs=9, spread=(0, 3), rad=(3.2, 1.2), by=(0, 14), squash=0.90),
    "alder":   dict(th=26, tr=2, bark=(100, 88, 70), canopy=[(70, 120, 48), (86, 136, 58), (56, 104, 40)], blobs=8, spread=(2, 6), rad=(4.0, 1.8), by=(1, 7), squash=0.85),
    "banyan":  dict(th=32, tr=3, bark=(88, 74, 56), canopy=[(46, 104, 40), (62, 126, 48), (38, 90, 36)], blobs=14, spread=(2, 12), rad=(6.0, 2.5), by=(0, 10), squash=0.95),
    "pine":    dict(th=52, tr=2, bark=(108, 72, 46), canopy=[(44, 92, 46), (56, 106, 54), (36, 80, 40)], blobs=0, spread=(0, 0), rad=(0, 0), by=(0, 0), squash=0.45),
    "dead":    dict(th=34, tr=2, bark=(118, 110, 98), canopy=[], blobs=0),
}


def tree_at(L, xm, zm, kind, seed):
    gy = terrain(xm, zm) - 0.02
    cx, cy0, cz = cell(xm, gy, zm)
    K = KINDS[kind]
    trunk_h, tr, bark = K["th"], K["tr"], K["bark"]
    canopy, blobs = K["canopy"], K["blobs"]
    th = int(trunk_h * (0.75 + 0.5 * h3(seed, 1, 1, SEED)))
    for dx in range(-4, 5):
        for dz in range(-4, 5):
            if dx * dx + dz * dz <= 16:
                for dy in range(-4, 1):
                    L.put(cx + dx, cy0 + dy, cz + dz, SOIL, (86, 60, 34),
                          refl=55, rough=225, amp=12, s=1)
    for i in range(th + 1):
        t = i / float(th)
        wx = cx + int(round(2.5 * t * math.sin(seed)))
        wz = cz + int(round(2.5 * t * math.cos(seed)))
        r = tr if i < th - 8 else max(1, tr - 1)
        for dx in range(-r, r + 1):
            for dz in range(-r, r + 1):
                if dx * dx + dz * dz <= r * r + 1:
                    L.put(wx + dx, cy0 + i, wz + dz, WOOD, bark, tex=T_BARK,
                          amp=14, s=2)
    top_y = cy0 + th
    for b in range(4 if kind != "dead" else 5):
        ang = seed + b * 2.4
        elev = 0.5 + 0.25 * h3(b, 0, 0, seed)
        bx, by, bz = cx, top_y - 6 + b * 2, cz
        ln = 8 + int(6 * h3(b, 1, 0, seed))
        for i in range(ln):
            bx += int(round(math.cos(ang) * 1.1))
            bz += int(round(math.sin(ang) * 1.1))
            by += int(round(elev * 1.0)) if i % 2 == 0 else 0
            r = 1 if i > ln // 2 else 2
            for dx in range(-r, r + 1):
                for dz in range(-r, r + 1):
                    L.put(bx + dx, by, bz + dz, WOOD, bark, tex=T_BARK,
                          amp=14, s=3)
    if kind == "dead":
        return
    if kind == "pine":
        for layer in range(6):
            ly = top_y - 4 + layer * 3
            r = max(2.0, 7.0 - layer * 0.9)
            L.blob(cx, ly, cz, r, FOLIAGE, canopy[layer % 3],
                   squash=0.45, amp=15, s=5 + layer)
        L.blob(cx, top_y + 12, cz, 2.2, FOLIAGE, canopy[0], squash=0.8,
               amp=15, s=9)
        return
    sp0, sp1 = K["spread"]
    r0, r1 = K["rad"]
    by0, by1 = K["by"]
    for b in range(blobs):
        ang = seed * 3 + b * (6.283 / blobs)
        rr = sp0 + sp1 * h3(b, 2, 0, seed)
        bx = cx + int(round(math.cos(ang) * rr))
        bz = cz + int(round(math.sin(ang) * rr))
        by = top_y + by0 + int(round((by1 - by0) * h3(b, 3, 0, seed)))
        r = r0 + r1 * h3(b, 4, 0, seed)
        tex = T_MOSS if h3(b, 5, 0, seed) < 0.18 else 0
        L.blob(bx, by, bz, r, FOLIAGE, canopy[b % len(canopy)],
               tex=tex, squash=K["squash"], amp=16, s=5)
    L.blob(cx, top_y + 6, cz, 4.0, FOLIAGE, canopy[1], squash=0.8, amp=16, s=6)


def palm(L, xm, zm, seed, hgt=None):
    gy = terrain(xm, zm) - 0.02
    cx, cy0, cz = cell(xm, gy, zm)
    hgt = hgt or (44 + int(h3(seed, 1, 0, SEED) * 22))       # 4.4 - 6.6 m
    lean = (h3(seed, 2, 0, SEED) - 0.5) * 0.5                 # lateral drift
    t = 0.5 + h3(seed, 7, 0, SEED) * 0.5
    wx, wz = cx, cz
    # ringed trunk with a slight S-curve, 1-cell core, root flare at the base
    for dx in range(-2, 3):
        for dz in range(-2, 3):
            if abs(dx) + abs(dz) <= 3:
                L.put(cx + dx, cy0, cz + dz, SOIL, (96, 78, 58), s=1)
    for i in range(hgt):
        u = i / float(hgt)
        wx = cx + int(round(lean * hgt * (u * u) * 0.6))
        wz = cz + int(round(math.sin(u * 3.1 + seed) * 3))
        r = 1
        for dx in range(-r, r + 1):
            for dz in range(-r, r + 1):
                if dx * dx + dz * dz <= r * r + 1:
                    col = (122, 96, 66) if i < hgt - 6 else (110, 88, 60)
                    L.put(wx + dx, cy0 + i, wz + dz, BARK, col, tex=T_BARK,
                          amp=10, s=13 + (i % 8))   # ringed segment bands
    top = cy0 + hgt
    # crown: 7-9 arching fronds (rachis poly-line keeps width, tip wisps)
    nfr = 7 + int(h3(seed, 3, 0, SEED) * 3)
    for f in range(nfr):
        ang = seed + f * (6.283 / nfr)
        fx_, fz_ = wx, wz
        ln = 16 + int(h3(f, 1, 0, seed) * 8)
        droop = 0.0
        for i in range(ln):
            u = i / float(ln)
            fx_ += int(round(math.cos(ang) * 1.05))
            fz_ += int(round(math.sin(ang) * 1.05))
            droop = -int(9 * u * u)                # arches over under its weight
            col = (44, 98, 40) if h3(f, 2, 0, seed) < 0.5 else (60, 122, 46)
            L.put(fx_, top + 2 + droop, fz_, FOLIAGE, col, amp=12, s=f * 7 + i)
            # leaflets widen the blade mid-frond, taper to a wisp at the tip
            if 0.25 < u < 0.85:
                half = int(1 + 2 * math.sin((u - 0.25) * 4.2))
                ox = int(round(math.sin(ang) * half))
                oz = int(round(-math.cos(ang) * half))
                for sgn in (-1, 1):
                    L.put(fx_ + sgn * ox, top + 2 + droop, fz_ + sgn * oz,
                          FOLIAGE, col, amp=12, s=f * 7 - i)
        L.put(fx_, top + 2 - 9, fz_, FOLIAGE, (36, 84, 34), s=f)
    # coconuts at the crown base
    for k in range(1 + int(h3(seed, 5, 0, SEED) * 3)):
        a = k * 2.1 + seed
        L.put(wx + int(round(math.cos(a) * 2)), top - 1,
              wz + int(round(math.sin(a) * 2)), WOOD, (108, 84, 52), s=77 + k)


def fallen_log(L, xm, zm, ang, ln, seed, rgb=(110, 78, 44), mossy=True):
    gy = terrain(xm, zm) - 0.04
    cx, cy0, cz = cell(xm, gy, zm)
    for i in range(ln):
        lx = cx + int(round(math.cos(ang) * i))
        lz = cz + int(round(math.sin(ang) * i))
        r = 2 if i < ln - 4 else 1
        for dx in range(-r, r + 1):
            for dy in range(0, 2 * r + 1):
                L.put(lx + dx, cy0 + 1 + dy, lz, WOOD, rgb, tex=T_BARK,
                      amp=13, s=13)
        if mossy and h3(i, 0, 0, seed) < 0.4:
            L.put(lx, cy0 + 2 + 2 * r, lz, FOLIAGE, (66, 108, 44),
                  tex=T_MOSS, refl=35, rough=235, s=14)


def d(x2, z2):
    return math.sqrt((x2 * x2) / (RX * RX) + (z2 * z2) / (RZ * RZ))


def clear_of_all(x, z, path_keep=2.2, cenote_keep=5.2, house_keep=6.5,
                 light_keep=8.0, jetty_keep=4.0,
                 pool_keep=4.6):
    H = terrain(x, z)
    if H < WATER + 0.9:
        return False
    if slope(x, z) > 0.85:
        return False
    if math.hypot(x - HEADLAND[0], z - HEADLAND[1]) < light_keep:
        return False
    if math.hypot(x - HOUSE_PAD[0][0], z - HOUSE_PAD[0][1]) < house_keep:
        return False
    if math.hypot(x - CENOTE[0], z - CENOTE[1]) < cenote_keep:
        return False
    if math.hypot(x - TIDAL[0], z - TIDAL[1]) < pool_keep:
        return False
    if path_dist(x, z) < path_keep:
        return False
    if H < 0.35 and -z > 14:      # keep the open beach bare
        return False
    if abs(x - JETTY_X) < jetty_keep and z < -15:
        return False
    return True


# ---------------------------------------------------------------- builders --
def lay_forest():
    """Dense jungle, split across TWO layers (isle_forest + isle_jungle)
    because vf_mcp write_object caps one call at 200k voxels (a 350k run
    came back truncated to exactly 200000 — the round number is the
    signature). ~44 trees total."""
    L = Layer("isle_forest")
    J = Layer("isle_jungle")
    kinds_cycle = ["banyan", "beech", "oak", "alder", "beech", "oak",
                   "banyan", "birch"]
    placed = []
    tries = 0

    def route():
        # greedy bin-packing: the next tree goes to the SMALLER layer, so
        # neither side can cross the 200k cap (a fixed alternation landed
        # 205k on forest because banyans are much bigger than birches)
        return L if len(L.cells) <= len(J.cells) else J

    while len(placed) < 44 and tries < 9000:
        tries += 1
        x = (h3(tries, 7, SEED, 2) - 0.5) * 80
        z = (h3(tries, 9, SEED, 4) - 0.5) * 64
        dens = max(0.0, 1.0 - d(x, z)) + (0.4 if z > 0 else 0.0)
        if h3(tries, 11, SEED, 5) > min(1.0, 0.30 + dens * 0.62):
            continue
        if not clear_of_all(x, z):
            continue
        H = terrain(x, z)
        if H > 10.0:              # above the treeline: bare cone rock
            continue
        if any(math.hypot(x - px, z - pz) < 4.2 for px, pz in placed):
            continue
        near_cone = math.hypot(x - (-6), z - 16) < 14 and H > 5.5
        kind = "pine" if near_cone and h3(tries, 3, SEED, 6) < 0.7 \
            else kinds_cycle[len(placed) % len(kinds_cycle)]
        tree_at(route(), x, z, kind, h3(tries, 5, SEED, 1) * 6.28)
        placed.append((x, z))
    # a dead snag stands by the cenote rim (gull perch, scale reference)
    tree_at(L, 9.6, 4.2, "dead", 2.77)
    return L, J


def lay_palms():
    L = Layer("isle_palms")
    spots = []
    # beach ring: an open grove belt behind the swash, left+right of the jetty
    for i in range(16):
        ang = -math.pi / 2 + (h3(i, 21, SEED, 8) - 0.5) * 2.5
        if math.sin(ang) > -0.22:
            continue
        rr = RX * (0.74 + h3(i, 23, SEED, 9) * 0.14)
        x = math.cos(ang) * rr
        z = math.sin(ang) * RZ * (0.74 + h3(i, 23, SEED, 9) * 0.14)
        if z > -10:
            continue
        spots.append((x, z))
    # cottage grove (behind the house, facing the bay)
    spots += [(-15.5, -9.5), (-16.8, -7.2), (-14.0, -3.0), (-16.0, -12.5),
              (-9.5, -12.0), (-11.5, -13.8)]
    # cenote / tidal pool accents
    spots += [(11.0, 8.2), (17.5, 8.0), (12.0, -13.8)]
    for i, (x, z) in enumerate(spots):
        H = terrain(x, z)
        if H < WATER + 1.0 or slope(x, z) > 0.7:
            continue
        if abs(x - JETTY_X) < 3.0 and z < -15:
            continue
        if math.hypot(x - HOUSE_PAD[0][0], z - HOUSE_PAD[0][1]) < 5.0:
            continue
        palm(L, x, z, i * 17.77)
    return L


def lay_rocks():
    L = Layer("isle_rocks")
    rock, rock2 = (84, 82, 78), (128, 126, 118)
    # headland boulders + moss caps
    for i in range(16):
        t = h3(i, 81, SEED, 27) * 6.283
        rr = 5.5 + h3(i, 83, SEED, 28) * 4.0
        x = HEADLAND[0] + math.cos(t) * rr
        z = HEADLAND[1] + math.sin(t) * rr
        H = terrain(x, z)
        if H < WATER + 0.15:
            continue
        cxp, cyp, czp = cell(x, H - 0.05, z)
        r = 3 + h3(i, 85, SEED, 29) * 4.5
        L.blob(cxp, cyp + int(r * 0.7), czp, r, ROCK if i % 2 else LROCK,
               rock if i % 2 else rock2, squash=0.8, amp=14, s=700 + i)
        if h3(i, 87, SEED, 30) < 0.6:
            L.blob(cxp + 1, cyp + int(r * 1.15), czp - 1, r * 0.55, FOLIAGE,
                   (60, 96, 40), tex=T_MOSS, squash=0.4, amp=12, s=740 + i)
    # beach pebble bands (swash line)
    for i in range(110):
        ang = -math.pi / 2 + (h3(i, 91, SEED, 31) - 0.5) * 2.4
        if math.sin(ang) > -0.2:
            continue
        rr = 0.93 + h3(i, 93, SEED, 32) * 0.06
        x = math.cos(ang) * RX * rr
        z = math.sin(ang) * RZ * rr
        H = terrain(x, z)
        if H < WATER + 0.12 or H > WATER + 0.85:
            continue
        cxp, cyp, czp = cell(x, H, z)
        r = 1 + h3(i, 95, SEED, 33)
        L.blob(cxp, cyp, czp, r, GRAVEL, (138, 132, 124), tex=T_GRAVEL,
               squash=0.55, amp=16, s=800 + i)
    # offshore reef rocks (break the sea silhouette)
    for (x, z, base_h, r) in [(33.5, -15.0, -1.8, 4.6), (-30.0, 11.5, -1.4, 3.8),
                              (20.5, -26.0, -2.2, 3.4)]:
        cxp, cyp, czp = cell(x, base_h, z)
        h0 = int((WATER - base_h) / VOXEL) + 5
        L.blob(cxp, cyp + h0, czp, r, ROCK, (72, 74, 70), squash=0.62,
               amp=15, s=900)
    return L


def lay_lighthouse():
    """Banded tower on a ROCK PLINTH that stands on the natural knoll —
    v3 rule: the landscape provides no pad, the building layer brings its
    own footing. Base sampled from the freshly baked heightmap."""
    L = Layer("isle_lighthouse")
    px, pz = HEADLAND
    g_ring = max(terrain(px + math.cos(a) * 3.2, pz + math.sin(a) * 3.2)
                 for a in [k * math.tau / 8 for k in range(8)])
    g_ctr = terrain(px, pz)
    base = max(g_ctr, 3.0) + 0.15          # threshold above local crown
    cx, cy0, cz = cell(px, base, pz)
    # weathered masonry plinth: stepped rock courses, wider as they descend,
    # from the threshold down past the LOWEST knoll corner (-0.4 embed)
    k = 0
    yy = cy0 - 1
    floor_bot = int((g_ring + 0.5 + HALF) / VOXEL)          # top course depth
    foot0 = int((g_ctr - 0.45 + HALF) / VOXEL)              # deepest needed
    while yy > foot0:
        r = 2.6 + 0.55 * k
        L.blob(cx, yy, cz, r, ROCK if k % 2 else LROCK,
               (92, 88, 82) if k % 2 else (124, 120, 112),
               squash=0.42, amp=13, s=1 + k)
        yy -= 3
        k += 1
    y = cy0 + 3
    H_TOWER = 118
    R0, R1 = 20, 13
    BAND = 30

    def r_at(i):
        r = R0 + (R1 - R0) * (i / float(H_TOWER))
        return int(round(r)) + int(1.6 * math.exp(-i / 30.0))

    # doorway (south face): open arch, no panel (articulation does not exist)
    def in_door(i, dx, dz):
        if not (2 <= i <= 30):
            return False
        r = r_at(i)
        return abs(dx) <= 5 and -(r + 1) <= dz <= -(r - 4)

    for i in range(H_TOWER):
        r = r_at(i)
        band = (i // BAND) % 2
        col = PLASTER if band == 0 else ROOF
        rgb = (244, 240, 232) if band == 0 else (96, 44, 40)
        for dx in range(-r - 1, r + 2):
            for dz in range(-r - 1, r + 2):
                dd = math.hypot(dx, dz)
                if not (dd <= r and dd > r - 3.5):
                    continue
                if in_door(i, dx, dz):
                    continue
                L.put(cx + dx, y + i, cz + dz, col, rgb,
                      amp=6 if band == 0 else 9, s=10 + i)
    # door frame + lintel + the open leaf ajar against the wall
    r0 = r_at(0)
    for i in range(0, 33):
        for sgn in (-6, 6):
            L.put(cx + sgn, y + i, cz - r0 - 1, WOOD, (78, 56, 38),
                  tex=T_BARK, s=60 + i)
    for dx in range(-6, 7):
        L.put(cx + dx, y + 32, cz - r0 - 1, WOOD, (78, 56, 38), s=61)
    for i in range(28):
        for dx in range(2, 6):
            L.put(cx + dx + 3, y + 2 + i // 2, cz - r0 - 2 + (dx - 2) // 2,
                  WOOD, (96, 70, 48), tex=T_BARK, amp=10, s=62 + i)
    # steps to the threshold, riding the knoll slope
    for sgn in range(3):
        sx_, sy_, sz_ = px + 4 - sgn * 1.2, base - 0.55 - sgn * 0.35, pz - r0 - 2 - sgn
        c = cell(sx_, max(sy_, terrain(sx_, sz_)), sz_)
        L.put(c[0], c[1], c[2], LROCK, (128, 126, 118), s=70 + sgn)
    # --- interior: spiral stair winding the hollow core
    inner_top = y + H_TOWER
    s = 0
    yy = y + 2
    wx = wz = cx
    while yy < inner_top - 2:
        th = s * 0.50
        i = yy - y
        ri = r_at(min(i, H_TOWER)) - 3.6
        if ri > 2:
            wx = cx + int(round(math.cos(th) * (ri - 1.4)))
            wz = cz + int(round(math.sin(th) * (ri - 1.4)))
            for kk in range(4):
                tx = wx + int(round(math.cos(th + 1.57) * (kk - 1.5)))
                tz = wz + int(round(math.sin(th + 1.57) * (kk - 1.5)))
                L.put(tx, yy, tz, WOOD, (98, 74, 50), amp=9, s=140 + s)
        if s % 26 == 25:
            for dx in range(-2, 3):
                for dz in range(-2, 3):
                    L.put(wx + dx, yy, wz + dz, WOOD, (90, 68, 46),
                          amp=8, s=150 + s)
        yy += 2
        s += 1
    # fuel store + storm lantern (ember mat: derived budget is seawater
    # noise; the REAL light is the authored beacon below the manifest)
    L.blob(cx + 6, y + 3, cz + 4, 2.6, WOOD, (96, 72, 48), squash=1.2, s=170)
    L.put(cx + 6, y + 6, cz + 4, EMBER, (255, 244, 228), refl=40,
          rough=150, s=171)
    # gallery deck + railing
    top = y + H_TOWER
    GR = R1 + 4
    for dx in range(-GR, GR + 1):
        for dz in range(-GR, GR + 1):
            if math.hypot(dx, dz) <= GR:
                if abs(dx) <= 4 and dz < -(GR - 3):
                    continue
                L.put(cx + dx, top, cz + dz, WOOD, (86, 62, 40), tex=T_BARK,
                      amp=8, s=77)
    for dx in range(-R1 - 1, R1 + 2):
        for dz in range(-R1 - 1, R1 + 2):
            if math.hypot(dx, dz) <= R1 + 1:
                L.put(cx + dx, top - 1, cz + dz, PLASTER, (244, 240, 232),
                      amp=6, s=78)
    for kk in range(28):
        a = kk / 28.0 * 6.283
        rx_, rz_ = int(round(math.cos(a) * (GR - 1))), int(round(math.sin(a) * (GR - 1)))
        L.column(cx + rx_, top + 1, cz + rz_, 4, WOOD, (72, 52, 36), s=90 + kk)
        L.put(cx + rx_, top + 4, cz + rz_, WOOD, (60, 44, 32), s=120 + kk)
    # lantern room: dark frame + warm glass (mat 10, see isle_interiors light)
    LR = 10
    LY = top + 1
    for i in range(16):
        for dx in range(-LR, LR + 1):
            for dz in range(-LR, LR + 1):
                dd = math.hypot(dx, dz)
                if dd <= LR and dd > LR - 3:
                    if i % 5 == 0 or dd > LR - 1.4:
                        L.put(cx + dx, LY + i, cz + dz, ROOF, (52, 48, 46),
                              amp=7, s=200 + i)
                    else:
                        L.put(cx + dx, LY + i, cz + dz, EMBER,
                              (255, 248, 238), refl=40, rough=140,
                              amp=2, s=210 + i)
    for i in range(10):
        r = int(LR + 2 - i * 1.1)
        for dx in range(-r, r + 1):
            for dz in range(-r, r + 1):
                if math.hypot(dx, dz) <= r:
                    L.put(cx + dx, LY + 16 + i, cz + dz, ROOF, (46, 44, 42),
                          amp=8, s=300 + i)
    L.column(cx, LY + 26, cz, 5, WOOD, (60, 50, 40), s=40)
    return L


def lay_jetty():
    L = Layer("isle_jetty")
    x = JETTY_X
    z_start, z_end = -19.5, -24.5

    def deckY(zm):
        return max(WATER + 0.42, terrain(x, zm) + 0.10)

    for zz in range(int((z_start + HALF) / VOXEL), int((z_end + HALF) / VOXEL) - 1, -1):
        zm = -HALF + zz * VOXEL
        bed = min(terrain(x, zm), WATER - 0.5)
        yy = int((deckY(zm) + HALF) / VOXEL)
        if (int((z_start + HALF) / VOXEL) - zz) % 15 == 0:
            for off in (-6, 6):
                cxp = int(round((x + off * VOXEL + HALF) / VOXEL))
                y0 = int((bed - 0.3 + HALF) / VOXEL)
                L.column(cxp, y0, zz, max(1, yy - 3 - y0), WOOD, (66, 52, 40),
                         tex=T_BARK, amp=12, s=zz)
        for off in range(-7, 8):
            cxp = int(round((x + off * VOXEL + HALF) / VOXEL))
            L.put(cxp, yy, zz, ROOF, (74, 60, 50) if off % 3 else (66, 54, 44),
                  amp=10, s=zz + off)
            if abs(off) == 7:
                L.put(cxp, yy + 1, zz, WOOD, (58, 46, 36), s=zz)
    for zz in range(int((z_start + HALF) / VOXEL), int((-22.0 + HALF) / VOXEL), -1):
        zm = -HALF + zz * VOXEL
        yy = int((deckY(zm) + HALF) / VOXEL)
        for off in (-7, 7):
            cxp = int(round((x + off * VOXEL + HALF) / VOXEL))
            L.column(cxp, yy + 1, zz, 7, WOOD, (62, 48, 38), s=zz + off)
        for off in range(-6, 7):
            cxp = int(round((x + off * VOXEL + HALF) / VOXEL))
            L.put(cxp, yy + 8, zz, WOOD, (62, 48, 38), s=zz)
    yy = int((deckY(z_end) + HALF) / VOXEL)
    cxp = int(round((x + 4 * VOXEL + HALF) / VOXEL))
    zze = int((z_end + 0.5 + HALF) / VOXEL)
    L.column(cxp, yy + 1, zze, 24, WOOD, (60, 46, 36), tex=T_BARK, s=700)
    L.put(cxp, yy + 26, zze, WHITEHOT, (255, 246, 235), refl=40, rough=140, s=701)
    for bx in (-3, 3):
        L.column(int(round((x + bx * VOXEL + HALF) / VOXEL)), yy + 1, zze + 4,
                 5, WOOD, (78, 58, 42), s=705)
    return L


def lay_driftwood():
    L = Layer("isle_driftwood")
    for i in range(8):
        ang = -math.pi / 2 + (h3(i, 101, SEED, 34) - 0.5) * 2.3
        if math.sin(ang) > -0.3:
            continue
        rr = 0.90 + h3(i, 103, SEED, 35) * 0.07
        x = math.cos(ang) * RX * rr
        z = math.sin(ang) * RZ * rr
        H = terrain(x, z)
        if H < WATER + 0.25:
            continue
        if abs(x - JETTY_X) < 2.6 and z < -15:
            continue
        fallen_log(L, x, z, h3(i, 105, SEED, 36) * 3.14,
                   8 + int(h3(i, 107, SEED, 37) * 12), i * 17.0,
                   rgb=(150, 140, 125), mossy=False)
    # weathered stakes flanking the beach access
    for k in range(6):
        t = k / 5.0
        x = PATH[2][0] + (PATH[1][0] - PATH[2][0]) * t + 1.3
        z = PATH[2][1] + (PATH[1][1] - PATH[2][1]) * t - 0.9
        H = terrain(x, z)
        cxp, cyp, czp = cell(x, H - 0.02, z)
        L.column(cxp, cyp, czp, 5 + (k % 2), WOOD, (140, 132, 120),
                 amp=13, s=900 + k)
    return L


# NO voxel small plants (user directive 2026-11-19): ground cover is the
# shading-level flora (terrain mats 0/1 -> applyFlora) + the incoming photo-
# card pass. Trees are large geometry, logs are wood props — both stay.
LAYERS = {
    "isle_forest": lay_forest,
    "isle_palms": lay_palms,
    "isle_rocks": lay_rocks,
    "isle_lighthouse": lay_lighthouse,
    "isle_jetty": lay_jetty,
    "isle_driftwood": lay_driftwood,
    # mesh hero props are imported by isle_meshes.sh, not authored here
}

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default=None, choices=sorted(LAYERS))
    args = ap.parse_args()
    for name in (args.only,) if args.only else LAYERS:
        res = LAYERS[name]()
        for layer in (res if isinstance(res, tuple) else (res,)):
            layer.write()
