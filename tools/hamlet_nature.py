#!/usr/bin/env python3
"""Author two new nature layers for the lakeside hamlet.

  * hamlet_forest : seven deciduous trees (oak / birch / maple + one dead
    snag), each with a solid trunk, branches and stacked solid canopy blobs
    (solid = the surfelizer's thin rule can never fire; also no flood-fill
    leaks). Placed on the west/north/east margins, clear of the buildings,
    the garden, the conifer grove and the live-edit test keep-out
    (world x -3..3, z -1..6).

  * hamlet_nature : undergrowth and props - bushes, ferns, flower patches,
    mushrooms, mossy boulders, fallen logs, stumps, grass tufts, a pulled-up
    canoe and a small standing-stone circle on the west meadow.

Everything is explicit 0.1 m cells written through vf_mcp write_object;
colours carry a deterministic positional jitter.
"""
import json
import math
import subprocess

import numpy as np
from PIL import Image

VOXEL = 0.1
WORLD = 102.4
HALF = 0.5 * WORLD

# Terrain sampler (bilinear over assets/heightmap.png, matches HeightMap).
_HM = np.asarray(Image.open("assets/heightmap.png"), dtype=np.float64)
_HM = -8.0 + (_HM / 65535.0) * 32.0


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


ROCK, LROCK, WOOD, FOLIAGE, SOIL = 4, 5, 6, 8, 2
T_BARK, T_MOSS = 17, 18
TEX_GRAVEL = 15          # atlas slot bound to textures/gravel.png in world.json
TEX_COBBLE = 14          # atlas slot bound to textures/light_rock.png (cobbles)


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
                    if (dx * dx + dz * dz + (dy / squash) ** 2) <= r * r:
                        self.put(cx + dx, cy + dy, cz + dz, mat, rgb,
                                 tex=tex, amp=amp, s=s)

    def write(self):
        out = list(self.cells.values())
        xs = [r["x"] for r in out]; ys = [r["y"] for r in out]
        zs = [r["z"] for r in out]
        print(f"{self.name}: {len(out)} voxels, bounds x[{min(xs)}..{max(xs)}] "
              f"y[{min(ys)}..{max(ys)}] z[{min(zs)}..{max(zs)}]")
        payload = {"name": self.name, "voxels": out}
        req = ("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
               "\"params\":{\"name\":\"write_object\",\"arguments\":"
               + json.dumps(payload, separators=(",", ":")) + "}}\n")
        p = subprocess.run(["./build/vf_mcp"], input=req, capture_output=True,
                           text=True, timeout=900)
        for line in p.stdout.splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                d = json.loads(line)
                txt = d.get("result", {}).get("content", [{}])[0].get("text", "")
                print("mcp:", txt[:300])
            except Exception:
                print("raw:", line[:200])


# ------------------------------------------------------------------ forest --
KINDS = {
    "oak":         dict(th=30, tr=2, bark=(108, 76, 42),  canopy=[(56, 118, 44), (72, 140, 52), (44, 100, 38)],  blobs=9,  spread=(2, 7),  rad=(4.5, 2.0), by=(2, 8),  squash=0.85),
    "birch":       dict(th=38, tr=1, bark=(226, 222, 210), canopy=[(96, 152, 60), (118, 168, 70), (80, 132, 52)], blobs=7,  spread=(2, 6),  rad=(3.5, 1.5), by=(2, 8),  squash=0.80),
    "maple":       dict(th=26, tr=2, bark=(96, 70, 46),   canopy=[(84, 138, 48), (104, 158, 60), (68, 116, 42)],  blobs=8,  spread=(2, 7),  rad=(4.0, 2.0), by=(1, 7),  squash=0.85),
    "beech":       dict(th=40, tr=2, bark=(126, 118, 104), canopy=[(58, 112, 44), (76, 132, 52), (46, 96, 38)],  blobs=11, spread=(2, 9),  rad=(5.0, 2.5), by=(1, 9),  squash=0.90),
    "willow":      dict(th=30, tr=2, bark=(118, 108, 88), canopy=[(112, 150, 66), (126, 162, 76), (98, 136, 58)], blobs=9,  spread=(3, 10), rad=(4.5, 2.5), by=(0, 4),  squash=0.65),
    "poplar":      dict(th=46, tr=2, bark=(152, 150, 140), canopy=[(84, 128, 52), (100, 142, 60), (70, 112, 44)], blobs=9,  spread=(0, 3),  rad=(3.2, 1.2), by=(0, 14), squash=0.90),
    "alder":       dict(th=26, tr=2, bark=(100, 88, 70),  canopy=[(70, 120, 48), (86, 136, 58), (56, 104, 40)],  blobs=8,  spread=(2, 6),  rad=(4.0, 1.8), by=(1, 7),  squash=0.85),
    "pine":        dict(th=52, tr=2, bark=(108, 72, 46),  canopy=[(44, 92, 46), (56, 106, 54), (36, 80, 40)],    blobs=0,  spread=(0, 0),  rad=(0.0, 0.0), by=(0, 0),  squash=0.45),
    "ancient_oak": dict(th=36, tr=3, bark=(94, 64, 34),   canopy=[(52, 108, 42), (68, 128, 50), (42, 92, 36)],   blobs=14, spread=(2, 12), rad=(6.0, 2.5), by=(0, 10), squash=0.95),
    "dead":        dict(th=34, tr=2, bark=(118, 110, 98), canopy=[], blobs=0),
}


def tree_at(L, xm, zm, ground_y, kind, seed):
    cx, cy0, cz = cell(xm, ground_y, zm)
    K = KINDS[kind]
    trunk_h, tr, bark = K["th"], K["tr"], K["bark"]
    canopy, blobs = K["canopy"], K["blobs"]

    # soil mound (sunk 4 cells, so slopes never leave a floating trunk)
    for dx in range(-4, 5):
        for dz in range(-4, 5):
            if dx * dx + dz * dz <= 16:
                for dy in range(-4, 1):
                    L.put(cx + dx, cy0 + dy, cz + dz, SOIL, (86, 60, 34),
                          refl=55, rough=225, amp=12, s=1)
    # trunk: slightly leaning, tapering
    for i in range(trunk_h + 1):
        t = i / float(trunk_h)
        wx = cx + int(round(2.5 * t * math.sin(seed)))
        wz = cz + int(round(2.5 * t * math.cos(seed)))
        r = tr if i < trunk_h - 8 else max(1, tr - 1)
        for dx in range(-r, r + 1):
            for dz in range(-r, r + 1):
                if dx * dx + dz * dz <= r * r + 1:
                    L.put(wx + dx, cy0 + i, wz + dz, WOOD, bark,
                          tex=T_BARK, amp=14, s=2)
    top_y = cy0 + trunk_h
    # branches
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
                    L.put(bx + dx, by, bz + dz, WOOD, bark,
                          tex=T_BARK, amp=14, s=3)
    if kind == "dead":
        return
    if kind == "pine":
        # layered conifer: flat dark tiers, tallest tree in the wood
        for layer in range(6):
            ly = top_y - 4 + layer * 3
            r = max(2.0, 7.0 - layer * 0.9)
            L.blob(cx, ly, cz, r, FOLIAGE, canopy[layer % 3],
                   squash=0.45, amp=15, s=5 + layer)
        L.blob(cx, top_y + 12, cz, 2.2, FOLIAGE, canopy[0], squash=0.8,
               amp=15, s=9)
        return
    # canopy: stacked solid blobs around the crown (species-shaped)
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
        col = canopy[b % len(canopy)]
        tex = T_MOSS if h3(b, 5, 0, seed) < 0.18 else 0
        L.blob(bx, by, bz, r, FOLIAGE, col, tex=tex, squash=K["squash"],
               amp=16, s=5)
    # crown cap
    L.blob(cx, top_y + 6, cz, 4.0, FOLIAGE, canopy[1], squash=0.8, amp=16, s=6)


def bush(L, xm, zm, ground_y, r, seed):
    cx, cy0, cz = cell(xm, ground_y, zm)
    greens = [(52, 104, 38), (70, 126, 46), (40, 88, 32)]
    for b in range(3):
        ang = seed + b * 2.1
        bx = cx + int(round(math.cos(ang) * r * 0.5))
        bz = cz + int(round(math.sin(ang) * r * 0.5))
        L.blob(bx, cy0 + 2 + b, bz, r - b * 0.6, FOLIAGE,
               greens[b % 3], squash=0.8, amp=14, s=7 + b)
    if h3(seed, 0, 0, 3) < 0.5:                    # berries
        for k in range(4):
            bx = cx + int(round((h3(k, 1, 0, seed) - 0.5) * 2 * r))
            bz = cz + int(round((h3(k, 2, 0, seed) - 0.5) * 2 * r))
            by = cy0 + 3 + int(round(h3(k, 3, 0, seed) * 3))
            L.put(bx, by, bz, FOLIAGE, (168, 40, 44), refl=40, rough=220)


def fern(L, xm, zm, ground_y, seed):
    cx, cy0, cz = cell(xm, ground_y, zm)
    col = (74, 138, 50)
    for f in range(7):
        ang = seed + f * 0.9
        fx, fz = cx, cz
        for i in range(5):
            fx += int(round(math.cos(ang) * 0.9))
            fz += int(round(math.sin(ang) * 0.9))
            L.put(fx, cy0 + 1 + i, fz, FOLIAGE, col, amp=14, s=8)
            if i == 2:
                L.put(fx + 1, cy0 + 3, fz, FOLIAGE, col, amp=14, s=8)


def flower_patch(L, xm, zm, ground_y, n, seed):
    cx, cy0, cz = cell(xm, ground_y, zm)
    palette = [(206, 60, 62), (232, 206, 84), (232, 232, 238),
               (92, 96, 190), (176, 96, 188)]
    for k in range(n):
        dx = int(round((h3(k, 1, 0, seed) - 0.5) * 2 * 7))
        dz = int(round((h3(k, 2, 0, seed) - 0.5) * 2 * 7))
        stem = 1 + int(h3(k, 3, 0, seed) * 3)
        for i in range(stem):
            L.put(cx + dx, cy0 + 1 + i, cz + dz, FOLIAGE, (86, 132, 54),
                  amp=10, s=9)
        col = palette[int(h3(k, 4, 0, seed) * len(palette)) % len(palette)]
        L.put(cx + dx, cy0 + 1 + stem, cz + dz, FOLIAGE, col,
              refl=40, rough=225, amp=12, s=10)
        if h3(k, 5, 0, seed) < 0.5:
            L.put(cx + dx + 1, cy0 + stem, cz + dz, FOLIAGE, col,
                  refl=40, rough=225, amp=12, s=10)


def mushroom(L, xm, zm, ground_y, seed):
    cx, cy0, cz = cell(xm, ground_y, zm)
    for k in range(3):
        dx = int(round((h3(k, 1, 0, seed) - 0.5) * 3))
        dz = int(round((h3(k, 2, 0, seed) - 0.5) * 3))
        L.put(cx + dx, cy0 + 1, cz + dz, FOLIAGE, (216, 206, 188),
              refl=45, rough=220, s=11)
        cap = (186, 58, 50) if h3(k, 3, 0, seed) < 0.6 else (198, 176, 148)
        L.put(cx + dx, cy0 + 2, cz + dz, FOLIAGE, cap, refl=45, rough=220,
              s=11)


def boulder(L, xm, zm, ground_y, r, seed):
    cx, cy0, cz = cell(xm, ground_y, zm)
    grey = (128, 124, 118) if h3(seed, 0, 0, 1) < 0.5 else (150, 146, 140)
    for dx in range(-int(r) - 1, int(r) + 2):
        for dy in range(-int(r) - 1, int(r) + 2):
            for dz in range(-int(r) - 1, int(r) + 2):
                d = math.sqrt(dx * dx + (dy * 1.25) ** 2 + dz * dz)
                if d <= r + 0.3 * h3(dx, dy, dz, seed):
                    mat = ROCK if d > r * 0.5 else LROCK
                    tex = T_MOSS if (dy > 0 and h3(dx, dy, dz, 2) < 0.35) else 0
                    L.put(cx + dx, cy0 + dy + int(r * 0.4), cz + dz, mat, grey,
                          refl=95, rough=150, tex=tex, amp=13, s=12)


def fallen_log(L, xm, zm, ground_y, ang, ln, seed):
    cx, cy0, cz = cell(xm, ground_y, zm)
    bark = (110, 78, 44)
    for i in range(ln):
        lx = cx + int(round(math.cos(ang) * i))
        lz = cz + int(round(math.sin(ang) * i))
        r = 2 if i < ln - 4 else 1
        for dx in range(-r, r + 1):
            for dy in range(0, 2 * r + 1):
                L.put(lx + dx, cy0 + 1 + dy, lz, WOOD, bark,
                      tex=T_BARK, amp=13, s=13)
        if h3(i, 0, 0, seed) < 0.4:                # moss on top
            L.put(lx, cy0 + 2 + 2 * r, lz, FOLIAGE, (66, 108, 44),
                  tex=T_MOSS, refl=35, rough=235, s=14)


def stump(L, xm, zm, ground_y, r, seed):
    cx, cy0, cz = cell(xm, ground_y, zm)
    for dy in range(0, 5):
        for dx in range(-r, r + 1):
            for dz in range(-r, r + 1):
                if dx * dx + dz * dz <= r * r + 1:
                    L.put(cx + dx, cy0 + dy, cz + dz, WOOD, (108, 78, 44),
                          tex=T_BARK, amp=13, s=15)
    for dx in range(-r, r + 1):                     # cut face
        for dz in range(-r, r + 1):
            if dx * dx + dz * dz <= r * r + 1:
                d = math.hypot(dx, dz)
                col = (198, 172, 122) if d > r * 0.55 else (176, 148, 104)
                L.put(cx + dx, cy0 + 5, cz + dz, WOOD, col, refl=70,
                      rough=150, amp=10, s=16)


def grass_tuft(L, xm, zm, ground_y, seed):
    cx, cy0, cz = cell(xm, ground_y, zm)
    col = (96, 158, 58)
    for k in range(3):
        dx = int(round((h3(k, 1, 0, seed) - 0.5) * 3))
        dz = int(round((h3(k, 2, 0, seed) - 0.5) * 3))
        h = 2 + int(h3(k, 3, 0, seed) * 2)
        for i in range(h):
            L.put(cx + dx, cy0 + 1 + i, cz + dz, FOLIAGE, col, amp=12, s=17)


def canoe(L, xm, zm, ground_y, ang, seed):
    """Pulled-up canoe: hollow hull with ribs, two thwarts, a paddle."""
    cx, cy0, cz = cell(xm, ground_y, zm)
    hull = (128, 92, 52)
    dark = (92, 64, 34)
    ln = 34
    for i in range(-ln // 2, ln // 2 + 1):
        t = abs(i) / (ln / 2.0)
        w = int(round(4.0 * (1.0 - 0.75 * t * t)))     # taper at bow/stern
        h = int(round(5.0 * (1.0 - 0.55 * t * t)))
        lx = cx + int(round(math.cos(ang) * i))
        lz = cz + int(round(math.sin(ang) * i))
        for dw in range(-w, w + 1):
            wx = lx - int(round(math.sin(ang) * dw))
            wz = lz + int(round(math.cos(ang) * dw))
            for dy in range(0, h):
                if abs(dw) >= w - 1 or dy == 0:        # hull shell + keel line
                    L.put(wx, cy0 + 1 + dy, wz, WOOD, hull if dy else dark,
                          tex=T_BARK, amp=12, s=18)
                elif h3(wx, dy, wz, seed) < 0.25:      # ribs inside
                    L.put(wx, cy0 + 1 + dy, wz, WOOD, dark, amp=10, s=19)
        if i in (-8, 8):                                # thwarts
            for dw in range(-w + 1, w):
                wx = lx - int(round(math.sin(ang) * dw))
                wz = lz + int(round(math.cos(ang) * dw))
                L.put(wx, cy0 + 4, wz, WOOD, (150, 112, 62), tex=T_BARK,
                      amp=10, s=20)
    # paddle leaning on the hull
    px, pz = cx + int(round(math.cos(ang) * 14)), cz + int(round(math.sin(ang) * 14))
    for i in range(14):
        L.put(px + i // 3, cy0 + 1 + i, pz, WOOD, (168, 130, 76),
              tex=T_BARK, amp=8, s=21)
    for dx in range(-1, 2):
        for dz in range(-1, 2):
            L.put(px + 5 + dx, cy0 + 15, pz + dz, WOOD, (168, 130, 76),
                  amp=8, s=21)


def stone_circle(L, xm, zm, ground_y, seed):
    cx, cy0, cz = cell(xm, ground_y, zm)
    n = 7
    for k in range(n):
        ang = k * 6.283 / n + seed
        sx = cx + int(round(math.cos(ang) * 25))
        sz = cz + int(round(math.sin(ang) * 25))
        h = 16 + int(h3(k, 0, 0, seed) * 8)
        w = 2 if k % 2 else 3
        for dy in range(0, h):
            taper = 1.0 - 0.25 * (dy / float(h))
            r = max(1, int(w * taper))
            for dx in range(-r, r + 1):
                for dz in range(-r, r + 1):
                    if dx * dx + dz * dz <= r * r + 1:
                        col = (140, 138, 132)
                        tex = T_MOSS if h3(dx, dy, dz, 3) < 0.30 else 0
                        L.put(sx + dx, cy0 + dy, sz + dz, LROCK, col,
                              refl=110, rough=140, tex=tex, amp=12, s=22)
    # one fallen stone
    for i in range(10):
        L.put(cx + 12 + i, cy0 + 1 + i // 4, cz + 20, LROCK, (134, 130, 124),
              refl=110, rough=140, tex=T_MOSS, amp=12, s=23)
    # low altar stone in the middle
    for dx in range(-3, 4):
        for dz in range(-3, 4):
            if dx * dx + dz * dz <= 10:
                L.put(cx + dx, cy0 + 1, cz + dz, LROCK, (148, 144, 138),
                      refl=110, rough=140, tex=T_MOSS, amp=12, s=24)


def litter(L, xm, zm, seed):
    """Dark moss / leaf-litter patch, flush with the terrain top."""
    cx, cy0, cz = cell(xm, terrain(xm, zm), zm)
    r = 2.5 + 2.5 * h3(seed, 0, 0, 55)
    for dx in range(-int(r) - 1, int(r) + 2):
        for dz in range(-int(r) - 1, int(r) + 2):
            d = math.hypot(dx, dz)
            if d > r + 0.8 * h3(dx, 0, dz, 56) or h3(dx, 1, dz, seed) < 0.25:
                continue
            wx = xm + dx * 0.1
            wz = zm + dz * 0.1
            ty = cell(wx, terrain(wx, wz), wz)[1]
            moss = h3(dx, 2, dz, seed) < 0.6
            col = (62, 90, 42) if moss else (76, 56, 36)
            L.put(cx + dx, ty, cz + dz, 18 if moss else 2, col,
                  refl=35, rough=235, tex=T_MOSS if moss else 0, amp=14, s=57)


def path(L, pts, seed, obstacles, tex=TEX_GRAVEL, r0=0.5, r1=0.7, plants=True):
    """Gravel/cobble way along a polyline: jittered discs of textured cells,
    flush with the terrain, a few raised stones, and (optionally) plants
    along the verges."""
    def skip_cell(cx, cz):
        wx, wz = (cx - 512) * 0.1, (cz - 512) * 0.1
        if blocked(wx, wz):
            return True
        if math.hypot(wx - TOWER[0], wz - TOWER[1]) < 2.95:
            return True
        # cabin / garden / boat footprints (a path must not cut through them)
        if 2.6 < wx < 8.6 and 8.1 < wz < 14.5:
            return True
        if 1.25 < wx < 10.85 and 15.35 < wz < 25.75:
            return True
        if 12.7 < wx < 15.5 and 1.0 < wz < 7.9:
            return True
        for (ox, oz, r) in obstacles:
            if math.hypot(wx - ox, wz - oz) < r:
                return True
        return False

    def stamp(px_, pz_):
        r = r0 + (r1 - r0) * h3(int(px_ * 10), 0, int(pz_ * 10), seed)
        cx_, _, cz_ = cell(px_, 0, pz_)
        ty = cell(px_, terrain(px_, pz_), pz_)[1]
        rr = int(r * 10) + 1
        for dx in range(-rr, rr + 1):
            for dz in range(-rr, rr + 1):
                if math.hypot(dx, dz) > r * 10 + 0.5 * h3(cx_ + dx, 7, cz_ + dz, seed):
                    continue
                wx, wz = cx_ + dx, cz_ + dz
                if skip_cell(wx, wz):
                    continue
                g = 150 if h3(wx, 3, wz, seed) < 0.5 else 122
                L.put(wx, ty, wz, LROCK, (g, g - 4, g - 11), refl=115,
                      rough=135, tex=tex, amp=16, s=58)
                if h3(wx, 5, wz, seed) < 0.07:      # raised stone
                    L.put(wx, ty + 1, wz, LROCK, (g + 22, g + 16, g + 8),
                          refl=115, rough=135, tex=tex, amp=16, s=59)

    total = 0
    for i in range(len(pts) - 1):
        (x0, z0), (x1, z1) = pts[i], pts[i + 1]
        n = max(1, int(math.hypot(x1 - x0, z1 - z0) / 0.2))
        for k in range(n + 1):
            t = k / float(n)
            stamp(x0 + (x1 - x0) * t, z0 + (z1 - z0) * t)
            total += 1
        # verge plants every ~2.5 m, alternating sides
        for k in range(0, n + 1, 12) if plants else []:
            t = k / float(n)
            vx = x0 + (x1 - x0) * t
            vz = z0 + (z1 - z0) * t
            if h3(int(vx * 10), 9, int(vz * 10), seed) < 0.5:
                continue
            side = 1.0 if (k // 12) % 2 == 0 else -1.0
            dxs = -(z1 - z0)
            dzs = (x1 - x0)
            ln = max(1e-6, math.hypot(dxs, dzs))
            ox = vx + side * dxs / ln * (1.2 + 0.9 * h3(k, 1, 0, seed))
            oz = vz + side * dzs / ln * (1.2 + 0.9 * h3(k, 2, 0, seed))
            if skip_cell(*cell(ox, 0, oz)[::2]):
                continue
            pick = h3(k, 3, 0, seed)
            if pick < 0.4:
                fern(L, ox, oz, terrain(ox, oz), seed=12.0 + k * 0.1)
            elif pick < 0.75:
                bush(L, ox, oz, terrain(ox, oz), 2.2, seed=13.0 + k * 0.1)
            else:
                flower_patch(L, ox, oz, terrain(ox, oz), 5, seed=14.0 + k * 0.1)
    return total


# -------------------------------------------------------------------- build --
nature = Layer("hamlet_nature")
forest = Layer("hamlet_forest")
forest2 = Layer("hamlet_forest2")

# Dense forest: a ring around the tower's clearing at (15, 14) plus belts on
# the west, north and south-east margins. Terrain is sampled per tree; spots
# below the waterline or inside the live-edit keep-out are skipped.
KEEP_OUT = (-3.0, 3.0, -1.0, 6.0)     # x0, x1, z0, z1
TOWER = (15.0, 14.0)


def blocked(x, z):
    x0, x1, z0, z1 = KEEP_OUT
    if x0 < x < x1 and z0 < z < z1:
        return True
    return terrain(x, z) < -0.5


def in_buildings(x, z):
    """Cabin and garden footprints (trees must not grow inside them)."""
    if 2.6 < x < 8.6 and 8.1 < z < 14.9:
        return True
    if 1.25 < x < 10.85 and 15.35 < z < 25.75:
        return True
    return False


TREES = [
    # ring around the tower clearing
    ("oak", 10.6, 10.2, 1.1), ("birch", 12.2, 15.4, 2.2), ("oak", 10.2, 17.0, 3.3),
    ("maple", 13.0, 19.0, 4.4), ("birch", 17.6, 20.6, 5.5), ("oak", 18.6, 17.4, 6.6),
    ("maple", 18.2, 10.6, 7.7), ("birch", 11.8, 8.6, 21.0),
    # west grove
    ("oak", -6.5, 13.5, 1.2), ("birch", -8.0, 9.5, 2.7), ("oak", -3.5, 20.0, 4.1),
    ("maple", -0.5, 27.0, 5.5), ("birch", -6.2, 24.0, 6.9), ("oak", -10.5, 16.0, 7.3),
    ("maple", -9.0, 20.5, 8.4), ("birch", -11.5, 11.0, 9.6), ("oak", -4.8, 8.8, 10.8),
    ("birch", -12.0, 24.0, 11.9), ("maple", -7.5, 28.0, 12.7),
    # north belt
    ("oak", 2.5, 29.0, 13.5), ("birch", 6.5, 30.5, 14.2), ("oak", 11.0, 29.0, 15.1),
    ("maple", 14.5, 27.5, 16.0),
    # south-east belt (below the conifer grove)
    ("oak", 18.0, 7.5, 3.3), ("dead", 24.0, 12.0, 6.0), ("birch", 28.5, 24.5, 1.9),
    ("oak", 26.0, 8.5, 17.2), ("maple", 30.0, 14.0, 18.3), ("birch", 24.5, 5.5, 19.4),
    # second ring / infill (denser forest)
    ("oak", 9.6, 9.6, 22.1), ("oak", 12.4, 9.0, 23.2), ("birch", 12.6, 7.8, 24.3),
    ("maple", 19.6, 9.4, 25.4), ("oak", 21.4, 10.6, 26.5),
    ("birch", 5.0, 26.5, 28.7), ("maple", 8.0, 27.0, 29.8), ("birch", -2.0, 24.0, 30.9),
    ("oak", 14.0, 22.0, 32.0), ("maple", 15.0, 24.0, 33.1), ("oak", 0.5, 10.0, 34.2),
    ("birch", -5.0, 10.5, 35.3), ("maple", -4.0, 27.5, 36.4), ("oak", -14.0, 14.0, 37.5),
    ("birch", -13.5, 20.0, 38.6), ("oak", -10.0, 26.0, 39.7), ("maple", 17.0, 30.0, 40.8),
    ("oak", 22.0, 30.5, 41.9), ("birch", 3.0, 32.0, 43.0), ("maple", -8.0, 32.0, 44.1),
    ("oak", -1.2, 21.0, 45.2),
]
placed = []
for kind, xm, zm, seed in TREES:
    if blocked(xm, zm) or in_buildings(xm, zm):
        print(f"tree {kind} at ({xm},{zm}) skipped (keep-out/underwater/buildings)")
        continue
    if math.hypot(xm - TOWER[0], zm - TOWER[1]) < 4.4:
        print(f"tree {kind} at ({xm},{zm}) skipped (tower clearing)")
        continue
    if any(math.hypot(xm - px, zm - pz) < 2.6 for (px, pz) in placed):
        print(f"tree {kind} at ({xm},{zm}) skipped (too close to another tree)")
        continue
    placed.append((xm, zm))
    tree_at(forest, xm, zm, terrain(xm, zm), kind, seed)
print(f"trees placed: {len(placed)}")

# forest floor litter: dark moss / leaf patches flush with the terrain under
# the canopy, so the forest floor reads darker and more forested
for i, (kind, xm, zm, seed) in enumerate(TREES):
    if (xm, zm) not in placed:
        continue
    for k in range(2):
        ox = xm + (h3(i, k, 11, 95) - 0.5) * 6.0
        oz = zm + (h3(i, k, 12, 96) - 0.5) * 6.0
        if blocked(ox, oz) or math.hypot(ox - TOWER[0], oz - TOWER[1]) < 3.6:
            continue
        litter(nature, ox, oz, seed=11.0 + i + k * 0.1)

# forest floor: ferns/bushes/mushrooms scattered under the new canopy
floor = 0
for i, (kind, xm, zm, seed) in enumerate(TREES):
    if (xm, zm) not in placed:
        continue
    for k in range(5):
        ox = xm + (h3(i, k, 1, 91) - 0.5) * 5.0
        oz = zm + (h3(i, k, 2, 92) - 0.5) * 5.0
        if blocked(ox, oz) or math.hypot(ox - TOWER[0], oz - TOWER[1]) < 3.4:
            continue
        gy = terrain(ox, oz)
        pick = h3(i, k, 3, 93)
        if pick < 0.45:
            fern(nature, ox, oz, gy, seed=8.0 + i + k * 0.1)
        elif pick < 0.8:
            bush(nature, ox, oz, gy, 2.4 + h3(i, k, 4, 94) * 1.2,
                 seed=9.0 + i + k * 0.1)
        else:
            mushroom(nature, ox, oz, gy, seed=10.0 + i + k * 0.1)
        floor += 1
print(f"forest floor items: {floor}")

# --- second wood: varied large trees (own layer, under the 200k write cap) --
TREES2 = [
    ("beech", -9.0, 8.5, 50.1), ("willow", -7.0, 7.6, 52.3),
    ("pine", -12.5, 17.0, 51.2), ("poplar", -15.0, 24.0, 53.4),
    ("alder", -3.0, 11.5, 54.5), ("ancient_oak", -6.0, 30.5, 55.6),
    ("beech", 2.0, 24.0, 56.7), ("pine", -1.0, 13.5, 57.8),
    ("poplar", 6.0, 23.5, 59.0), ("alder", 11.6, 24.6, 60.1),
    ("ancient_oak", 12.0, 7.0, 62.3), ("pine", 8.0, 6.5, 63.4),
    ("poplar", 22.5, 6.5, 64.5), ("beech", 23.8, 9.2, 65.6),
    ("alder", 28.5, 7.5, 66.7), ("pine", 30.5, 10.5, 68.9),
    ("beech", -16.0, 30.0, 70.0), ("poplar", -10.5, 34.0, 71.1),
    ("ancient_oak", 5.5, 33.5, 72.2), ("alder", 16.0, 32.0, 73.3),
    ("pine", 26.0, 31.0, 74.4), ("willow", 30.0, 20.0, 67.8),
    ("beech", 20.0, 31.5, 75.5),
    ("beech", -4.0, 31.5, 76.1), ("poplar", -12.0, 29.0, 77.2),
    ("pine", 8.5, 32.5, 78.3), ("alder", 19.5, 33.0, 79.4),
    ("poplar", 26.5, 10.5, 80.5), ("beech", 29.5, 12.5, 81.6),
    ("pine", 31.5, 8.5, 82.7), ("ancient_oak", 25.0, 5.5, 83.8),
    ("beech", 13.8, 25.5, 84.9), ("willow", 7.0, 33.0, 86.0),
    ("pine", -2.5, 35.5, 87.1), ("alder", -16.5, 20.0, 88.2),
    ("poplar", -18.0, 27.0, 89.3), ("beech", 34.0, 22.0, 90.4),
]
placed2 = []
for kind, xm, zm, seed in TREES2:
    if blocked(xm, zm) or in_buildings(xm, zm):
        print(f"tree {kind} at ({xm},{zm}) skipped (keep-out/underwater/buildings)")
        continue
    if math.hypot(xm - TOWER[0], zm - TOWER[1]) < 4.4:
        print(f"tree {kind} at ({xm},{zm}) skipped (tower clearing)")
        continue
    if any(math.hypot(xm - px, zm - pz) < 2.3 for (px, pz) in placed + placed2):
        print(f"tree {kind} at ({xm},{zm}) skipped (too close to another tree)")
        continue
    placed2.append((xm, zm))
    tree_at(forest2, xm, zm, terrain(xm, zm), kind, seed)
print(f"large trees placed: {len(placed2)}")
for i, (kind, xm, zm, seed) in enumerate(TREES2):
    if (xm, zm) not in placed2:
        continue
    for k in range(2):
        ox = xm + (h3(i, k, 21, 97) - 0.5) * 6.0
        oz = zm + (h3(i, k, 22, 98) - 0.5) * 6.0
        if blocked(ox, oz) or math.hypot(ox - TOWER[0], oz - TOWER[1]) < 3.6:
            continue
        litter(nature, ox, oz, seed=21.0 + i + k * 0.1)
    for k in range(3):
        ox = xm + (h3(i, k, 23, 99) - 0.5) * 5.0
        oz = zm + (h3(i, k, 24, 100) - 0.5) * 5.0
        if blocked(ox, oz) or math.hypot(ox - TOWER[0], oz - TOWER[1]) < 3.4:
            continue
        gy = terrain(ox, oz)
        pick = h3(i, k, 25, 101)
        if pick < 0.45:
            fern(nature, ox, oz, gy, seed=22.0 + i + k * 0.1)
        elif pick < 0.8:
            bush(nature, ox, oz, gy, 2.4 + h3(i, k, 26, 102) * 1.2,
                 seed=23.0 + i + k * 0.1)
        else:
            mushroom(nature, ox, oz, gy, seed=24.0 + i + k * 0.1)

BUSHES = [(2.2, 9.8, 3.2), (8.4, 8.2, 3.0), (1.5, 16.5, 3.4),
          (-1.5, 12.0, 3.0), (11.4, 15.1, 3.2), (17.4, 10.8, 3.6),
          (4.5, 15.5, 2.8), (-4.5, 9.5, 3.0), (20.0, 9.0, 3.4),
          (-6.0, 18.0, 3.2), (-8.5, 21.0, 3.6), (10.0, 8.6, 2.6)]
for i, (xm, zm, r) in enumerate(BUSHES):
    bush(nature, xm, zm, terrain(xm, zm), r, seed=1.3 * i)

FERNS = [(9.5, 9.2), (15.0, 14.5), (-7.0, 7.8), (20.0, 17.0), (-2.5, 10.5),
         (6.0, 9.2), (16.5, 13.0), (13.5, 12.5)]
for i, (xm, zm) in enumerate(FERNS):
    if math.hypot(xm - TOWER[0], zm - TOWER[1]) < 3.0:
        continue                       # keep the tower clearing open
    fern(nature, xm, zm, terrain(xm, zm), seed=2.2 * i)

FLOWERS = [(3.0, 14.0, 8), (-2.5, 15.0, 7), (8.0, 14.6, 9),
           (-5.0, 12.5, 6), (12.4, 17.4, 7), (17.8, 12.2, 8),
           (-3.0, 24.0, 7), (7.5, 27.0, 6)]
for i, (xm, zm, n) in enumerate(FLOWERS):
    flower_patch(nature, xm, zm, terrain(xm, zm), n, seed=3.1 * i)

MUSHROOMS = [(-6.0, 12.0), (-3.0, 18.5), (17.5, 8.5), (23.0, 12.5)]
for i, (xm, zm) in enumerate(MUSHROOMS):
    mushroom(nature, xm, zm, terrain(xm, zm), seed=4.4 * i)

ROCKS = [(-6.5, 13.0, 4), (17.5, 9.0, 3), (3.0, 18.0, 2),
         (-9.0, 12.0, 5), (20.5, 20.0, 4), (-2.0, 8.6, 2)]
for i, (xm, zm, r) in enumerate(ROCKS):
    boulder(nature, xm, zm, terrain(xm, zm), r, seed=5.0 + i)

fallen_log(nature, 11.8, 20.4, terrain(11.8, 20.4), 0.5, 16, seed=1.0)
fallen_log(nature, -5.5, 16.0, terrain(-5.5, 16.0), 2.2, 13, seed=2.0)
stump(nature, 11.6, 14.4, terrain(11.6, 14.4), 3, seed=1.0)
stump(nature, -2.0, 18.5, terrain(-2.0, 18.5), 2, seed=2.0)

for i in range(30):
    xm = -12.0 + 44.0 * h3(i, 1, 0, 77)
    zm = 7.0 + 25.0 * h3(i, 2, 0, 77)
    if blocked(xm, zm):
        continue
    grass_tuft(nature, xm, zm, terrain(xm, zm), seed=6.0 + i)

canoe(nature, -4.5, 10.5, terrain(-4.5, 10.5), 1.35, seed=7.0)
stone_circle(nature, -6.5, 18.5, terrain(-6.5, 18.5), seed=0.6)

# gravel path: cabin door -> tower door, then a loop around the tower
# clearing, a tail north through the trees and a branch west to the stones
MAIN = [(5.2, 14.8), (6.6, 14.6), (7.8, 14.0), (8.8, 13.4), (9.8, 12.8),
        (10.8, 12.2), (11.9, 11.0), (13.0, 12.0)]
RING = [(13.0, 12.0), (13.6, 10.2), (15.0, 9.8), (16.8, 10.4), (18.0, 11.0),
        (19.2, 12.6), (19.4, 14.0), (18.6, 15.8), (17.4, 17.0), (15.0, 18.2),
        (13.2, 18.0), (12.0, 17.0)]
NORTH = [(12.0, 17.0), (11.2, 18.6), (11.2, 21.0), (11.4, 23.6), (11.8, 26.2),
         (12.6, 28.2), (13.6, 30.0)]
BRANCH = [(7.8, 14.0), (6.0, 14.6), (4.2, 15.1), (2.4, 15.2), (0.6, 16.4),
          (-1.2, 17.4), (-3.0, 18.4), (-4.6, 18.9), (-5.8, 18.6)]
LINK = [(15.0, 9.8), (15.2, 8.6)]        # ring -> street connector
# cobblestone street along the river (wider, cobble texture, no verge plants)
STREET = [(-10.0, 9.4), (-8.0, 9.5), (-6.0, 9.4), (-4.5, 8.8), (-2.5, 9.4),
          (-1.0, 10.6), (0.5, 11.6), (2.0, 11.0), (4.0, 9.8), (6.0, 9.0),
          (8.0, 8.8), (10.0, 9.2), (12.0, 8.6), (14.0, 7.6), (16.0, 7.3),
          (18.0, 7.8), (20.0, 9.0), (21.5, 9.7)]
OBSTACLES = ([(px, pz, 1.3) for (px, pz) in placed + placed2]
             + [(11.6, 14.4, 1.3), (11.8, 20.4, 1.6), (-5.5, 16.0, 1.4),
                (-6.5, 13.0, 2.2), (17.5, 9.0, 1.9), (3.0, 18.0, 1.4),
                (-9.0, 12.0, 2.5), (20.5, 20.0, 2.2), (-2.0, 8.6, 1.4),
                (-4.5, 10.5, 1.8), (-6.5, 18.5, 2.4)])
st = 0
for pts, sd, pl in ((MAIN, 15.0, True), (RING, 15.5, True), (NORTH, 16.0, True),
                    (BRANCH, 16.5, True), (LINK, 17.0, False)):
    st += path(nature, pts, seed=sd, obstacles=OBSTACLES, plants=pl)
st += path(nature, STREET, seed=18.0, obstacles=OBSTACLES,
           tex=TEX_COBBLE, r0=1.0, r1=1.3, plants=False)
print(f"path stamps: {st}")

nature.write()
forest.write()
forest2.write()
