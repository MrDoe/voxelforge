#!/usr/bin/env python3
"""Rebuild assets/CabinPart1.vxw as a detailed log cabin.

The old layer was a single cabin-kit mesh part whose voxelisation produced a
plain hollow box (all mat 6, no openings, no roof detail). This replaces it
with a hand-authored cabin at the same site (lattice centre 568/624, base
row 504):

  * stone foundation with a rubble look, projecting under the log walls
  * stacked log walls (3-cell period: 2-cell log + recessed pale chinking)
    with alternating corner log-ends (end grain + pith) and per-course colour
  * board-and-batten door with iron hinges/latch, stone threshold
  * four windows (two on the entry wall) with dark glazing, timber mullions
    and open shutters; flower boxes under the entry windows
  * gable roof with exposed rafter tails, coursed shingle skin, moss patches,
    ridge cap and gable bargeboards; stone chimney with a cap slab
  * porch (two posts + shed roof), bench, barrels, chopping block, woodpile
  * interior seen through the openings: plank floor, stone hearth with ember
    fire (mat 10), cauldron, table + stools, straw bed, chest, lantern
  * mossy stone path from the door, ivy on the west gable, flower bed

Deterministic: positional hash for all variation.
"""
import json
import math
import subprocess

VOXEL = 0.1
WORLD = 102.4
HALF = 0.5 * WORLD

X0, X1 = 546, 589        # log wall outer lines (x)
Z0, Z1 = 601, 645        # log wall outer lines (z)
YF = 510                 # floor level row (top of foundation = floor boards)
YB = 504                 # foundation sole (world -0.8)
YTOP = 537               # top log course row (eaves)

RIDGE_Z = (Z0 + Z1) // 2
Y_RIDGE = 556
ROOF_X0, ROOF_X1 = 542, 593
ROOF_Z0, ROOF_Z1 = 596, 650

DOOR_X0, DOOR_X1 = 559, 568
DOOR_TOP = 529
WIN_N = [(551, 556, 516), (572, 577, 516)]      # north wall: (x0,x1,sill)
WIN_W = (630, 635, 516)                         # west gable (z range)
WIN_E = (612, 617, 516)                         # east gable (z range)

ROCK, LROCK, WOOD, SHINGLE, FOLIAGE = 4, 5, 6, 7, 8
M_STONE = dict(mat=ROCK, rgb=[122, 118, 108], refl=95, rough=150)
M_STONE_D = dict(mat=ROCK, rgb=[96, 92, 84], refl=95, rough=155)
M_STONE_L = dict(mat=LROCK, rgb=[162, 158, 148], refl=115, rough=135)
M_MOSS = dict(mat=18, rgb=[74, 96, 52], refl=35, rough=235)
M_OAK = dict(mat=WOOD, rgb=[128, 86, 46], refl=70, rough=155)
M_OAK_D = dict(mat=WOOD, rgb=[98, 64, 32], refl=70, rough=160)
M_OAK_L = dict(mat=WOOD, rgb=[152, 108, 58], refl=70, rough=140)
M_PLANK = dict(mat=WOOD, rgb=[140, 98, 54], refl=70, rough=155)
M_END = dict(mat=WOOD, rgb=[196, 166, 116], refl=70, rough=150)
M_CHINK = dict(mat=WOOD, rgb=[178, 162, 130], refl=55, rough=195)
M_SHINGLE = dict(mat=SHINGLE, rgb=[100, 72, 42], refl=60, rough=170)
M_SHINGLE_D = dict(mat=SHINGLE, rgb=[78, 56, 32], refl=60, rough=170)
M_SHINGLE_L = dict(mat=SHINGLE, rgb=[118, 88, 52], refl=60, rough=170)
M_EMBER = dict(mat=10, rgb=[255, 120, 30], refl=45, rough=205)
M_COAL = dict(mat=9, rgb=[220, 70, 16], refl=45, rough=205)
M_GLASS = dict(mat=LROCK, rgb=[58, 72, 84], refl=185, rough=45)
M_IRON = dict(mat=ROCK, rgb=[54, 52, 58], refl=150, rough=90)
M_GRASS = dict(mat=FOLIAGE, rgb=[54, 110, 38], refl=30, rough=235)
M_STRAW = dict(mat=FOLIAGE, rgb=[184, 160, 92], refl=30, rough=235)
T_PLANK, T_BARK, T_MOSS = 6, 17, 18


def h3(x, y, z, s=0):
    n = (x * 73856093) ^ (y * 19349663) ^ (z * 83492791) ^ (s * 2654435761)
    n &= 0xFFFFFFFF
    n = (n ^ (n >> 13)) * 1274126177 & 0xFFFFFFFF
    return ((n ^ (n >> 16)) & 0xFFFF) / 65536.0


def jit(rgb, x, y, z, amp=9, s=0):
    d = int((h3(x, y, z, s) - 0.5) * 2.0 * amp)
    return [max(0, min(255, c + d)) for c in rgb]


cells = {}


def put(x, y, z, m, **over):
    if not (0 <= x < 1024 and 0 <= y < 1024 and 0 <= z < 1024):
        return
    rec = {"x": x, "y": y, "z": z, "mat": m["mat"]}
    rec["r"], rec["g"], rec["b"] = jit(m["rgb"], x, y, z,
                                       amp=over.pop("amp", 9))
    rec["refl"], rec["rough"] = m["refl"], m["rough"]
    rec.update(over)
    cells[(x, y, z)] = rec


def carve(x, y, z):
    cells.pop((x, y, z), None)


# ---------------------------------------------------------- 1. foundation --
for x in range(X0 - 1, X1 + 2):
    for z in range(Z0 - 1, Z1 + 2):
        edge = (x in (X0 - 1, X1 + 1) or z in (Z0 - 1, Z1 + 1))
        for y in range(YB, YF):
            if edge or y >= YF - 2:
                m = M_STONE_D if h3(x, y, z, 1) < 0.35 else M_STONE
                if h3(x, y, z, 2) < 0.12:
                    m = M_STONE_L
                put(x, y, z, m, amp=12)
# floor boards
for x in range(X0 + 2, X1 - 1):
    for z in range(Z0 + 2, Z1 - 1):
        m = M_PLANK if (x + z) % 2 == 0 else M_OAK_L
        put(x, YF, z, m, tex=T_PLANK)

# ---------------------------------------------------------- 2. log walls ---
def wall_runs():
    """(axis, fixed, lo, hi, inward) for the four wall faces."""
    yield ('z', Z0, X0, X1, +1)   # south wall, inward = +z
    yield ('z', Z1, X0, X1, -1)   # north wall
    yield ('x', X0, Z0, Z1, +1)   # west gable
    yield ('x', X1, Z0, Z1, -1)   # east gable


COURSES = [(YF + 3 * k, 2) for k in range((YTOP - YF) // 3 + 1)]
for ci, (ylo, h) in enumerate(COURSES):
    off = 1 if ci % 2 == 0 else 0
    m_log = (M_OAK if ci % 3 == 0 else
             (M_OAK_D if ci % 3 == 1 else M_OAK_L))
    for axis, fixed, lo, hi, inward in wall_runs():
        for t in range(lo - 1, hi + 2):
            for dy in range(h):
                y = ylo + dy
                if y > YTOP:
                    continue
                if axis == 'z':
                    base = fixed + inward * off
                    depth = [base, base + inward * 1, base + inward * 2]
                    pos = [(t, y, d) for d in depth]
                else:
                    base = fixed + inward * off
                    depth = [base, base + inward * 1, base + inward * 2]
                    pos = [(d, y, t) for d in depth]
                # opening cuts (door + windows) handled at the end
                for (x, yy, z) in pos:
                    put(x, yy, z, m_log, tex=T_BARK, amp=12)


# ---- chinking rows: recessed pale line at every course boundary -----------
for ci in range(len(COURSES) - 1):
    y = COURSES[ci][0] + COURSES[ci][1]         # boundary row
    off = 1 if ci % 2 == 0 else 0
    for axis, fixed, lo, hi, inward in wall_runs():
        for t in range(lo - 1, hi + 2):
            base = fixed + inward * off
            if axis == 'z':
                put(t, y, base, M_CHINK, amp=12)
            else:
                put(base, y, t, M_CHINK, amp=12)

# ---- corner log ends, alternating by course -------------------------------
for ci, (ylo, h) in enumerate(COURSES):
    m_log = M_END if ci % 3 == 0 else M_OAK_L
    for cy in range(h):
        y = ylo + cy
        if y > YTOP:
            continue
        ends = []
        if ci % 2 == 0:            # x-walls' logs extend past the corners
            ends = [(X0 - 2, y, Z0), (X0 - 2, y, Z1),
                    (X1 + 1, y, Z0), (X1 + 1, y, Z1)]
        else:                      # z-walls' logs extend
            ends = [(X0, y, Z0 - 2), (X0, y, Z1 + 1),
                    (X1, y, Z0 - 2), (X1, y, Z1 + 1)]
        for (x, yy, z) in ends:
            for dx in range(0, 3):
                for dz in range(0, 3):
                    if abs(dx - dz) <= 1:
                        put(x + (dx if x >= X1 else -dx), yy,
                            z + (dz if z >= Z1 else -dz), m_log,
                            tex=T_BARK, amp=10)
        # end-grain face cells (pale ring + dark pith)
        if ci % 2 == 0:
            for (ex, ez) in ((X0 - 2, Z0), (X0 - 2, Z1),
                             (X1 + 2, Z0), (X1 + 2, Z1)):
                put(ex, y, ez, M_END, amp=14)
                if cy == 0:
                    put(ex, y, ez + 1, M_OAK_D)
        else:
            for (ex, ez) in ((X0, Z0 - 2), (X0, Z1 + 2),
                             (X1, Z0 - 2), (X1, Z1 + 2)):
                put(ex, y, ez, M_END, amp=14)
                if cy == 0:
                    put(ex + 1, y, ez, M_OAK_D)

# ---------------------------------------------------------- 3. openings ----
def clear_box(x0, x1, y0, y1, z0, z1):
    for x in range(x0, x1 + 1):
        for y in range(y0, y1 + 1):
            for z in range(z0, z1 + 1):
                carve(x, y, z)


# door: through the north wall (z walls near Z1, depth Z1-2..Z1)
clear_box(DOOR_X0, DOOR_X1, YF, DOOR_TOP, Z1 - 3, Z1 + 1)
for y in range(YF + 1, DOOR_TOP + 1):                 # plank door leaf
    for x in range(DOOR_X0 + 1, DOOR_X1):
        m = M_PLANK if ((x - DOOR_X0) // 2) % 2 == 0 else M_OAK
        put(x, y, Z1 - 1, m, tex=T_PLANK)
for y in (YF + 3, YF + 4, YF + 15, YF + 16):          # iron straps
    for x in range(DOOR_X0 + 1, DOOR_X1):
        put(x, y, Z1, M_IRON)
put(DOOR_X1 - 2, YF + 9, Z1, M_IRON, refl=190, rough=60)   # latch
for y in (YF, DOOR_TOP + 1):                          # threshold + lintel log
    for x in range(DOOR_X0 - 1, DOOR_X1 + 2):
        for z in range(Z1 - 2, Z1 + 1):
            put(x, y, z, M_STONE_L if y == YF else M_OAK_D, tex=T_BARK if y != YF else 0)

# windows: north wall pair
for (wx0, wx1, sill) in WIN_N:
    clear_box(wx0, wx1, sill, sill + 7, Z1 - 3, Z1 + 1)
    for x in range(wx0 - 1, wx1 + 2):                 # sill + lintel
        for z in range(Z1 - 1, Z1 + 2):
            put(x, sill - 1, z, M_STONE_L)
            put(x, sill + 8, z, M_OAK_D, tex=T_BARK)
    for x in range(wx0, wx1 + 1):                     # glazing + mullion
        for y in range(sill, sill + 8):
            if x == (wx0 + wx1) // 2:
                put(x, y, Z1 - 2, M_OAK_D, tex=T_PLANK)
            else:
                put(x, y, Z1 - 2, M_GLASS)
    # open shutters, angled out on both sides
    for side, hx in ((0, wx0 - 1), (1, wx1 + 1)):
        for t in range(0, 6):
            xx = hx + (-t if side == 0 else t)
            zz = Z1 + 1 + t // 2
            for y in range(sill, sill + 8):
                put(xx, y, zz, M_OAK_D, tex=T_PLANK)
            for y in (sill + 1, sill + 6):
                put(xx, y, zz + 1, M_IRON)

# gable windows
for (fixed_axis, fixed, wz0, wz1, sill) in (('x', X0, WIN_W[0], WIN_W[1], WIN_W[2]),
                                            ('x', X1, WIN_E[0], WIN_E[1], WIN_E[2])):
    clear_box(fixed - 3, fixed + 1, sill, sill + 7, wz0, wz1)
    inward = 1 if fixed == X0 else -1
    for z in range(wz0, wz1 + 1):
        for y in range(sill, sill + 8):
            if z == (wz0 + wz1) // 2:
                put(fixed - inward * 2, y, z, M_OAK_D, tex=T_PLANK)
            else:
                put(fixed - inward * 2, y, z, M_GLASS)
    for z in range(wz0 - 1, wz1 + 2):                 # sill + lintel
        put(fixed, sill - 1, z, M_STONE_L)
        put(fixed, sill + 8, z, M_OAK_D, tex=T_BARK)

# ------------------------------------------------------------- 4. roof -----
# gable triangles (plank infill) + roof skin, ridge along X
def roof_y(z):
    d = abs(z - RIDGE_Z)
    span = (ROOF_Z1 - ROOF_Z0) / 2.0
    return Y_RIDGE - int(round((Y_RIDGE - YTOP) * d / span))


for x in (X0, X1):
    for z in range(Z0, Z1 + 1):
        for y in range(YTOP + 1, roof_y(z)):
            put(x, y, z, M_PLANK if h3(x, y, z, 8) > 0.3 else M_OAK_L,
                tex=T_PLANK, amp=11)
for z in range(ROOF_Z0, ROOF_Z1 + 1):
    ys = roof_y(z)
    # shingle skin, 5 cells deep: a 3-cell skin's cells run short in Y and the
    # surfelizer's thin rule would classify the sloped surface as thin
    # (narrow ellipses -> pinhole rows along every course). The two lowest
    # cells are the plank ceiling seen from inside.
    band = (ys // 4) % 3
    m = M_SHINGLE_D if band == 0 else (M_SHINGLE_L if band == 1 else M_SHINGLE)
    for x in range(ROOF_X0, ROOF_X1 + 1):
        for y in range(ys - 4, ys + 1):
            if y <= ys - 3:
                put(x, y, z, M_PLANK, tex=T_PLANK)
                continue
            mm = m
            if z > RIDGE_Z and y <= YTOP + 6 and h3(x, y, z, 9) < 0.10:
                mm = M_MOSS
            put(x, y, z, mm, tex=T_MOSS if mm is M_MOSS else (7 if h3(x, y, z, 5) > 0.2 else 0))
# rafter tails under the eaves
for x in range(ROOF_X0 + 2, ROOF_X1 - 1, 5):
    for z in (ROOF_Z0, ROOF_Z1):
        inward = 1 if z == ROOF_Z1 else -1
        for y in (YTOP - 2, YTOP - 1):
            put(x, y, z, M_OAK_D, tex=T_BARK)
            put(x + 1, y, z, M_OAK_D, tex=T_BARK)
# ridge cap + bargeboards
for x in range(ROOF_X0 - 1, ROOF_X1 + 2):
    for z in (RIDGE_Z - 1, RIDGE_Z, RIDGE_Z + 1):
        put(x, Y_RIDGE, z, M_SHINGLE_D, tex=7)
        put(x, Y_RIDGE + 1, z, M_SHINGLE_D if z == RIDGE_Z else M_SHINGLE, tex=7)
for x in (X0 - 1, X1 + 1):
    for z in range(ROOF_Z0, ROOF_Z1 + 1):
        y = roof_y(z)
        put(x, y, z, M_OAK_D, tex=T_BARK)
        put(x, y + 1, z, M_OAK_D if z % 2 else M_OAK, tex=T_BARK)

# chimney: stone stack on the west end, exits just south of the ridge
for x in range(X0 + 1, X0 + 6):
    for z in range(RIDGE_Z + 2, RIDGE_Z + 7):
        for y in range(YB, 566):
            if h3(x, y, z, 10) > 0.5 and y < 560:
                pass
            m = M_STONE_D if h3(x, y, z, 11) < 0.3 else M_STONE
            if y > Y_RIDGE:
                m = M_STONE
            put(x, y, z, m, amp=13)
for x in range(X0, X0 + 7):                            # cap slab + soot
    for z in range(RIDGE_Z + 1, RIDGE_Z + 8):
        put(x, 566, z, M_STONE_L, amp=10)
        put(x, 567, z, M_STONE_D, amp=8)
for x in range(X0 + 2, X0 + 5):
    for z in range(RIDGE_Z + 3, RIDGE_Z + 6):
        carve(x, 567, z)
        put(x, 567, z, dict(mat=ROCK, rgb=[30, 28, 30], refl=60, rough=200))

# ------------------------------------------------------- 5. porch + props --
for px in (DOOR_X0 - 1, DOOR_X1 + 2):                  # posts
    for y in range(YF, 535):
        put(px, y, Z1 + 5, M_OAK_D, tex=T_BARK, amp=12)
for z in range(Z1, Z1 + 6):                            # shed roof
    y = 535 - (z - Z1) // 2
    for x in range(DOOR_X0 - 3, DOOR_X1 + 4):
        put(x, y, z, M_PLANK, tex=T_PLANK)
        if h3(x, y, z, 12) > 0.35:
            put(x, y + 1, z, M_SHINGLE_D, tex=7)
for x in range(DOOR_X0 - 3, DOOR_X1 + 4):              # beam + bench
    put(x, 532, Z1 + 6, M_OAK_D, tex=T_BARK)
for x in range(DOOR_X1 + 3, DOOR_X1 + 10):
    for z in range(Z1 + 1, Z1 + 4):
        put(x, YF + 2, z, M_PLANK, tex=T_PLANK)
for (lx, lz) in ((DOOR_X1 + 3, Z1 + 1), (DOOR_X1 + 9, Z1 + 1),
                 (DOOR_X1 + 3, Z1 + 3), (DOOR_X1 + 9, Z1 + 3)):
    for y in range(YF, YF + 2):
        put(lx, y, lz, M_OAK_D, tex=T_BARK)
# lantern by the door (emissive)
put(DOOR_X1 + 1, YF + 8, Z1 + 1, M_IRON)
put(DOOR_X1 + 1, YF + 9, Z1 + 1, M_EMBER)

# woodpile against the west end of the north wall: ends face the entry
for k in range(5):
    y = YB + 2 + k * 2
    for x in range(548, 557):
        if (x + k) % 7 == 0:
            continue
        for z in range(Z1 + 1, Z1 + 5):
            m = M_END if z == Z1 + 4 else M_OAK_D
            put(x, y, z, m, tex=T_BARK, amp=11)
        put(x, y + 1, Z1 + 4, M_END, amp=14)
# chopping block + axe
for x in range(557, 561):
    for z in range(Z1 + 2, Z1 + 5):
        for y in range(YB + 2, YF + 4):
            put(x, y, z, M_OAK_D, tex=T_BARK)
for (ax, ay, az) in ((558, YF + 4, Z1 + 3), (559, YF + 5, Z1 + 3)):
    put(ax, ay, az, M_OAK, tex=T_BARK)
put(558, YF + 6, Z1 + 3, M_IRON, refl=170, rough=70)

# barrels by the east wall
for (bx, bz) in ((X1 + 2, Z1 - 4), (X1 + 2, Z1 - 7)):
    for dy in range(0, 5):
        r = 2.4 if dy in (1, 3) else 1.8
        for dx in range(-3, 4):
            for dz in range(-3, 4):
                if math.hypot(dx, dz) <= r:
                    put(bx + dx, YF - 2 + dy, bz + dz, M_OAK_D,
                        tex=T_BARK if dy in (1, 3) else 0)

# ------------------------------------------------------ 6. interior props --
# hearth against the west wall under the chimney
for x in range(X0 + 1, X0 + 6):
    for z in range(RIDGE_Z + 1, RIDGE_Z + 8):
        put(x, YF + 1, z, M_STONE_D, amp=12)
clear_box(X0 + 1, X0 + 4, YF + 2, YF + 4, RIDGE_Z + 2, RIDGE_Z + 6)
for x in range(X0 + 1, X0 + 5):                        # fire + cauldron
    for z in range(RIDGE_Z + 2, RIDGE_Z + 7):
        put(x, YF + 2, z, M_COAL, amp=20)
        if x == X0 + 2 and RIDGE_Z + 3 <= z <= RIDGE_Z + 5:
            put(x, YF + 3, z, M_EMBER)
put(X0 + 2, YF + 4, RIDGE_Z + 4, M_IRON)
put(X0 + 2, YF + 5, RIDGE_Z + 4, M_IRON)
# table + stools
for x in range(566, 576):
    for z in range(614, 620):
        put(x, YF + 5, z, M_OAK, tex=T_PLANK)
for (lx, lz) in ((567, 615), (574, 615), (567, 618), (574, 618)):
    for y in range(YF + 1, YF + 5):
        put(lx, y, lz, M_OAK_D, tex=T_BARK)
for (sx, sz) in ((564, 612), (578, 622)):
    for (dx, dz) in ((0, 0), (2, 0), (0, 2), (2, 2)):
        put(sx + dx, YF + 1, sz + dz, M_OAK_D, tex=T_BARK)
    put(sx + 1, YF + 3, sz + 1, M_OAK, tex=T_PLANK)
# straw bed + chest at the east end
for x in range(580, 588):
    for z in range(605, 614):
        put(x, YF + 1, z, M_STRAW, amp=16)
for x in range(580, 588):
    put(x, YF + 1, 604, M_OAK_D, tex=T_BARK)
for x in range(578, 584):
    for z in range(630, 636):
        for y in range(YF + 1, YF + 4):
            put(x, y, z, M_OAK_D, tex=T_PLANK)
for x in range(578, 584):
    put(x, YF + 4, 630, M_IRON)
# interior lantern on the north wall
put(576, YF + 11, Z1 - 3, M_OAK_D, tex=T_PLANK)
put(576, YF + 12, Z1 - 3, M_EMBER)

# ------------------------------------------------------- 7. exterior dress --
for x in range(X0 - 2, X1 + 3):                        # moss at the base
    for z in range(Z0 - 2, Z1 + 3):
        if (x in range(X0 - 1, X1 + 2) and z in range(Z0 - 1, Z1 + 2)):
            continue
        if h3(x, 0, z, 13) < 0.20:
            put(x, YF - 1, z, M_MOSS, tex=T_MOSS)
for z in range(Z0, Z1 + 1):                            # ivy on the west gable
    for x in (X0 - 1, X0 - 2):
        top = 6 + int(20 * h3(x, 0, z, 14))
        for y in range(YF, YTOP if top > 20 else YF + top):
            if h3(x, y, z, 15) < 0.75:
                put(x, y, z, M_GRASS, amp=16)
for cx in range(570, 579):                             # flower boxes + blooms
    if h3(cx, 0, 0, 16) < 0.3:
        continue
    put(cx, YF + 9, Z1 + 1, M_OAK_D, tex=T_PLANK)
    if h3(cx, 1, 0, 17) < 0.6:
        col = ([210, 70, 70] if h3(cx, 2, 0, 18) < 0.4 else
               ([235, 210, 90] if h3(cx, 2, 0, 19) < 0.5 else [225, 225, 235]))
        put(cx, YF + 10, Z1 + 1, dict(mat=FOLIAGE, rgb=col, refl=35, rough=230))
# mossy stepping stones to the village
px, pz = 565, Z1 + 7
for i in range(6):
    px += (1 if h3(i, 0, 0, 20) < 0.5 else 0) - (1 if h3(i, 1, 0, 20) < 0.3 else 0)
    pz += 2
    for dx in range(-1, 2):
        for dz in range(-1, 2):
            if h3(px + dx, 0, pz + dz, 21) < 0.7:
                put(px + dx, YF - 2, pz + dz,
                    M_STONE_L if h3(px, dx, dz, 22) < 0.4 else M_STONE_D)
                if h3(px, dx, dz, 23) < 0.25:
                    put(px + dx, YF - 1, pz + dz, M_MOSS, tex=T_MOSS)

out = list(cells.values())
print(f"{len(out)} voxels")
xs = [r["x"] for r in out]; ys = [r["y"] for r in out]; zs = [r["z"] for r in out]
print(f"bounds x[{min(xs)}..{max(xs)}] y[{min(ys)}..{max(ys)}] "
      f"z[{min(zs)}..{max(zs)}]  size {(max(xs)-min(xs)+1)*0.1:.1f}x"
      f"{(max(ys)-min(ys)+1)*0.1:.1f}x{(max(zs)-min(zs)+1)*0.1:.1f} m")

payload = {"name": "hamlet_cabin", "voxels": out}
req = ("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
       "\"params\":{\"name\":\"write_object\",\"arguments\":"
       + json.dumps(payload, separators=(",", ":")) + "}}\n")
print("payload bytes:", len(req))
p = subprocess.run(["./build/vf_mcp"], input=req, capture_output=True,
                   text=True, timeout=900)
for line in p.stdout.splitlines():
    line = line.strip()
    if not line:
        continue
    try:
        d = json.loads(line)
        txt = d.get("result", {}).get("content", [{}])[0].get("text", "")
        print("mcp:", txt[:500])
    except Exception:
        print("raw:", line[:300])
if p.stderr:
    print("stderr:", p.stderr[:400])
