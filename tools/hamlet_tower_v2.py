#!/usr/bin/env python3
"""Rebuild assets/hamlet_tower.vxw as a detailed round watchtower.

Replaces the old solid stone drum (no interior) with a real tower:

  * battered plinth, coursed ashlar shaft with pilasters + string courses
  * arched doorway (iron-bound plank door), arrow slits, six arched windows
    with stone sills, dark glazing and timber mullions
  * HOLLOW interior, wooden floor decks, and a continuous stone spiral
    staircase winding around a central newel from the ground floor to the
    battlement walk (each deck opens where the flight arrives)
  * corbelled wall head, crenellated parapet (merlons + embrasures),
    shingled conical spire set behind the battlements, iron finial + pennant
  * emissive lanterns (mat 10 ember) so the interior reads through the door
    and windows; ivy + moss on the base; interior props

The layer keeps its absolute bounding box (centre cell 711/691, base row
516), so the manifest `pos` keeps placing it exactly where it is.

Deterministic: per-cell colour comes from a positional hash, never an RNG.
"""
import json
import math
import subprocess

VOXEL = 0.1
WORLD = 102.4
HALF = 0.5 * WORLD

CX, CZ = 711, 691          # tower centre (lattice cells); matches old layer
Y0 = 516                   # base row (world y -0.4 = local terrain)

R_OUT = 24                 # shaft outer radius (2.4 m)
R_IN = 19                  # shaft inner radius  (1.9 m), 0.5 m wall
R_NEWEL = 2.5              # central stair newel radius

DOOR_A = 225.0             # door faces SW, toward the village/camera
DOOR_HALF = 13.0
DOOR_TOP = 21              # 2.1 m tall
ARCH_TOP = 26              # pointed arch head

DECK1, DECK2, WALK = 34, 62, 90
CORBEL0, CORBEL1 = 86, 89
PARAPET0, PARAPET1 = 90, 96
MERLON0, MERLON1 = 96, 101
CONE0, CONE1 = 96, 119
SPIRE_R = 21.5

ROCK, LROCK, WOOD, SHINGLE, FOLIAGE = 4, 5, 6, 7, 8
IRON = dict(mat=ROCK, rgb=[52, 50, 56], refl=150, rough=80)
M_STONE = dict(mat=ROCK, rgb=[128, 124, 114], refl=95, rough=150)
M_STONE_D = dict(mat=ROCK, rgb=[104, 100, 92], refl=95, rough=155)
M_STONE_L = dict(mat=LROCK, rgb=[168, 164, 154], refl=115, rough=135)
M_MOSS = dict(mat=18, rgb=[74, 96, 52], refl=35, rough=235)
M_OAK = dict(mat=WOOD, rgb=[124, 82, 44], refl=70, rough=150)
M_OAK_D = dict(mat=WOOD, rgb=[92, 60, 30], refl=70, rough=160)
M_OAK_L = dict(mat=WOOD, rgb=[150, 106, 58], refl=70, rough=140)
M_PLANK = dict(mat=WOOD, rgb=[138, 96, 52], refl=70, rough=155)
M_SHINGLE = dict(mat=SHINGLE, rgb=[102, 74, 44], refl=60, rough=170)
M_SHINGLE_D = dict(mat=SHINGLE, rgb=[80, 58, 34], refl=60, rough=170)
M_SHINGLE_L = dict(mat=SHINGLE, rgb=[120, 90, 54], refl=60, rough=170)
M_EMBER = dict(mat=10, rgb=[255, 120, 30], refl=45, rough=205)
M_GLASS = dict(mat=LROCK, rgb=[62, 76, 86], refl=185, rough=45)
M_GRASS = dict(mat=FOLIAGE, rgb=[52, 108, 36], refl=30, rough=235)
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


def put(dx, dy, dz, m, **over):
    x, y, z = CX + dx, Y0 + dy, CZ + dz
    if not (0 <= x < 1024 and 0 <= y < 1024 and 0 <= z < 1024):
        return
    rec = {"x": x, "y": y, "z": z, "mat": m["mat"]}
    rec["r"], rec["g"], rec["b"] = jit(m["rgb"], x, y, z,
                                       amp=over.pop("amp", 9))
    rec["refl"], rec["rough"] = m["refl"], m["rough"]
    rec.update(over)
    cells[(dx, dy, dz)] = rec


def in_ang(a, c, half):
    ta = (a - c) % 360.0
    return ta <= half or ta >= 360.0 - half


def ang_of(dx, dz):
    return math.degrees(math.atan2(dz, dx)) % 360.0


def cil(dx, dz):
    return math.hypot(dx, dz)


def shell(dy0, dy1, r0, r1, m, skip=None, **over):
    rr = int(max(r0, r1)) + 1
    lo, hi = min(r0, r1), max(r0, r1)
    for dy in range(dy0, dy1 + 1):
        for dx in range(-rr, rr + 1):
            for dz in range(-rr, rr + 1):
                r = cil(dx, dz)
                if lo <= r <= hi:
                    if skip and skip(dx, dy, dz, ang_of(dx, dz), r):
                        continue
                    put(dx, dy, dz, m, **over)


def disc(dy0, dy1, r, m, hole=None, ring_lo=0.0, **over):
    rr = int(r) + 1
    for dy in range(dy0, dy1 + 1):
        for dx in range(-rr, rr + 1):
            for dz in range(-rr, rr + 1):
                r_ = cil(dx, dz)
                if ring_lo <= r_ <= r:
                    if hole and hole(dx, dz, ang_of(dx, dz), r_):
                        continue
                    put(dx, dy, dz, m, **over)


# ------------------------------------------------------------- 1. plinth ----
shell(-2, -1, R_OUT + 2.0, R_OUT + 2.0, M_STONE_D)      # footing ring
disc(-1, -1, R_OUT + 2.0, M_STONE_D)                    # solid footing slab
for dy in range(0, 4):                                  # plinth: wall band only
    for dx in range(-30, 31):
        for dz in range(-30, 31):
            r = cil(dx, dz)
            if not (R_IN - 0.5 <= r <= R_OUT + 1.5):
                continue
            if dy >= 1 and in_ang(ang_of(dx, dz), DOOR_A, 10.0):
                continue                                # leave the doorway clear
            put(dx, dy, dz, M_STONE_L if dy == 3 else M_STONE)
disc(0, 0, R_OUT + 1.5, M_STONE_L, hole=None)           # plinth cap / floor

# -------------------------------------------------------- 2. main shaft ----
def shaft_skip(dx, dy, dz, a, r):
    if r < R_IN - 0.5:
        return True
    if in_ang(a, DOOR_A, DOOR_HALF):
        if 1 <= dy <= DOOR_TOP:
            return True
        if DOOR_TOP < dy <= ARCH_TOP:
            t = (dy - DOOR_TOP) / float(ARCH_TOP - DOOR_TOP)
            return in_ang(a, DOOR_A, DOOR_HALF * (1.0 - 0.85 * t)) and r > R_IN - 2
    for sa in (110, 290, 20):                    # ground-floor arrow slits
        if in_ang(a, sa, 2.5) and 9 <= dy <= 15 and r > R_IN - 3:
            return True
    for sa in (75, 195, 315):                    # first-floor windows
        if in_ang(a, sa, 9.5) and 40 <= dy <= 53 and r > R_IN - 3:
            return True
        if in_ang(a, sa, 9.5) and 53 < dy <= 58:
            t = (dy - 53) / 5.0
            return in_ang(a, sa, 9.5 * (1.0 - 0.8 * t)) and r > R_IN + 1
    for sa in (30, 150, 270):                    # second-floor windows
        if in_ang(a, sa, 9.5) and 68 <= dy <= 81 and r > R_IN - 3:
            return True
        if in_ang(a, sa, 9.5) and 81 < dy <= 86:
            t = (dy - 81) / 5.0
            return in_ang(a, sa, 9.5 * (1.0 - 0.8 * t)) and r > R_IN + 1
    return False


shell(0, CORBEL0 - 1, 22.0, R_OUT + 0.5, M_STONE, skip=shaft_skip)
shell(7, CORBEL0 - 1, R_IN - 1, R_IN + 0.5, M_STONE_L, skip=shaft_skip)

# pilasters: four shallow vertical strips, proud by one cell
for pa in (45, 135, 315):
    for dy in range(4, CORBEL0):
        for dx in range(-30, 31):
            for dz in range(-30, 31):
                r = cil(dx, dz)
                if R_OUT < r <= R_OUT + 1.0 and in_ang(ang_of(dx, dz), pa, 4.0):
                    put(dx, dy, dz, M_STONE_L if (dy // 6) % 2 else M_STONE_D)

# string courses at each floor line + corbelled wall head
for cy in (DECK1 - 2, DECK2 - 2):
    shell(cy, cy + 1, R_OUT, R_OUT + 1.2, M_STONE_L)
for dy in range(CORBEL0, CORBEL1 + 1):                  # corbel: grows outward
    t = (dy - CORBEL0) / float(CORBEL1 - CORBEL0)
    r_hi = R_OUT + 3.2 * t
    for dx in range(-30, 31):
        for dz in range(-30, 31):
            if R_OUT <= cil(dx, dz) <= r_hi:
                put(dx, dy, dz, M_STONE_D)
for dx in range(-30, 31):                               # corbel top cap
    for dz in range(-30, 31):
        if R_OUT <= cil(dx, dz) <= R_OUT + 3.2:
            put(dx, CORBEL1, dz, M_STONE_L)

# ------------------------------------------------------- 3. door + frames --
# clear the door pocket, then rebuild the surround from the arch curve so no
# spandrel is left open (the old jamb-only fill left gaps above the jambs)
for dx in range(-30, 31):
    for dz in range(-30, 31):
        a = ang_of(dx, dz)
        if not in_ang(a, DOOR_A, DOOR_HALF + 1.6):
            continue
        for dy in range(1, ARCH_TOP + 2):
            cells.pop((dx, dy, dz), None)

for dy in range(1, ARCH_TOP + 2):
    if dy <= DOOR_TOP:
        open_half = DOOR_HALF - 1.0
    else:
        t = (dy - DOOR_TOP) / float(ARCH_TOP - DOOR_TOP)
        open_half = DOOR_HALF * (1.0 - 0.85 * t)
    for dx in range(-30, 31):
        for dz in range(-30, 31):
            a = ang_of(dx, dz)
            r = cil(dx, dz)
            if not in_ang(a, DOOR_A, DOOR_HALF + 1.6):
                continue
            if not (R_IN - 2.0 <= r <= R_OUT + 0.5):
                continue
            if in_ang(a, DOOR_A, open_half):
                continue                             # the opening stays clear
            if in_ang(a, DOOR_A, open_half + 1.8):
                m = M_STONE_L if (dy // 3) % 2 == 0 else M_STONE_D
            else:
                m = M_STONE
            put(dx, dy, dz, m)
# keystone (2x2 boss on the arch apex)
for kx in (-18, -17):
    for kz in (-18, -17):
        put(kx, ARCH_TOP + 1, kz, M_STONE_L)

# plank door, closed, recessed one cell into the reveal; iron furniture
door_r = R_OUT - 2.0
for t in range(-int(DOOR_HALF * 2), int(DOOR_HALF * 2) + 1):
    a = DOOR_A + t * 0.5
    if not in_ang(a, DOOR_A, DOOR_HALF - 1.2):
        continue
    ddx = int(round(door_r * math.cos(math.radians(a))))
    ddz = int(round(door_r * math.sin(math.radians(a))))
    plank = M_PLANK if (t // 2) % 2 == 0 else M_OAK
    for dy in range(1, DOOR_TOP):
        put(ddx, dy, ddz, plank, tex=T_PLANK)
for hy in (4, 5, 17, 18):                            # hinge straps
    for t in range(0, int(DOOR_HALF * 2)):
        a = DOOR_A + t * 0.5
        if not in_ang(a, DOOR_A, DOOR_HALF - 2.0):
            continue
        ddx = int(round((door_r + 1) * math.cos(math.radians(a))))
        ddz = int(round((door_r + 1) * math.sin(math.radians(a))))
        put(ddx, hy, ddz, IRON)
# pointed door leaf: fill the arch head above the door top, so the doorway is
# a closed leaf and NOT an open slot showing the dark interior
for dy in range(DOOR_TOP, ARCH_TOP + 1):
    t = (dy - DOOR_TOP) / float(ARCH_TOP - DOOR_TOP)
    half = DOOR_HALF * (1.0 - 0.85 * t)
    for t2 in range(-int(DOOR_HALF * 2), int(DOOR_HALF * 2) + 1):
        a = DOOR_A + t2 * 0.5
        if not in_ang(a, DOOR_A, max(0.4, half - 0.3)):
            continue
        ddx = int(round(door_r * math.cos(math.radians(a))))
        ddz = int(round(door_r * math.sin(math.radians(a))))
        put(ddx, dy, ddz, M_PLANK, tex=T_PLANK)
hx = int(round((door_r + 1) * math.cos(math.radians(DOOR_A))))
hz = int(round((door_r + 1) * math.sin(math.radians(DOOR_A))))
for dy in (11, 12):
    put(hx, dy, hz, IRON, refl=190, rough=60)

# ----------------------------------------------------- 4. floors + stairs --
disc(1, 1, R_IN, M_STONE_L, amp=12)                  # ground-floor flagstones

def deck_hole(a_arr, half=37.5):
    def hole(dx, dz, ang, r):
        if r < R_NEWEL + 1:
            return False
        return in_ang(ang, a_arr, half)
    return hole


def spiral(y_from, y_to, a0, step=2, dA=21.0):
    """Helical ramp of 4-cell slabs (tread + overlap), newel to inner wall."""
    n = max(1, int(round((y_to - y_from) / float(step))))
    for k in range(n):
        ya = y_from + k * step
        aa = (a0 + k * dA) % 360.0
        for dx in range(-R_IN, R_IN + 1):
            for dz in range(-R_IN, R_IN + 1):
                r = cil(dx, dz)
                if r < R_NEWEL or r > R_IN + 0.2:
                    continue
                if (ang_of(dx, dz) - aa) % 360.0 > dA + 1.6:
                    continue
                for dy in range(ya - 1, ya + step + 2):
                    put(dx, dy, dz, M_OAK, tex=T_PLANK if (dy % 4) < 2 else 0)
    return (a0 + n * dA) % 360.0


for dy in range(1, WALK):                            # central newel
    for dx in range(-3, 4):
        for dz in range(-3, 4):
            if cil(dx, dz) <= R_NEWEL:
                put(dx, dy, dz, M_STONE_L if (dy // 8) % 2 else M_STONE_D)
disc(WALK, WALK, R_NEWEL + 1.5, M_OAK_D)             # oak newel cap

a1 = spiral(1, DECK1, 258.0)
a2 = spiral(DECK1, DECK2, (a1 + 40.0) % 360.0)
a3 = spiral(DECK2, WALK, (a2 + 40.0) % 360.0)

disc(DECK1 - 1, DECK1 - 1, R_IN - 1, M_OAK_D, hole=deck_hole(a1))
disc(DECK1, DECK1, R_IN - 1, M_PLANK, hole=deck_hole(a1), tex=T_PLANK)
disc(DECK2 - 1, DECK2 - 1, R_IN - 1, M_OAK_D, hole=deck_hole(a2))
disc(DECK2, DECK2, R_IN - 1, M_PLANK, hole=deck_hole(a2), tex=T_PLANK)
disc(WALK, WALK, R_IN + 2.0, M_PLANK, hole=deck_hole(a3), tex=T_PLANK)

# --------------------------------------------------- 5. battlements + roof --
shell(PARAPET0, PARAPET1, R_IN + 1.5, R_OUT + 1.0, M_STONE)   # parapet ring
for dx in range(-30, 31):                                     # merlons
    for dz in range(-30, 31):
        r = cil(dx, dz)
        if R_IN + 1.5 <= r <= R_OUT + 1.0:
            a = ang_of(dx, dz)
            if (a % 26.0) < 13.0:
                for dy in range(MERLON0, MERLON1 + 1):
                    put(dx, dy, dz, M_STONE_L if dy == MERLON1 else M_STONE)
            else:
                put(dx, PARAPET1 + 1, dz, M_STONE_L)          # embrasure cap
for sa in (90, 270, 0, 180):                                  # embrasure slits
    for dy in range(PARAPET0 + 3, PARAPET0 + 7):
        for dx in range(-30, 31):
            for dz in range(-30, 31):
                if in_ang(ang_of(dx, dz), sa, 2.6) and cil(dx, dz) >= R_IN + 1.4:
                    cells.pop((dx, dy, dz), None)

# conical spire, set behind the parapet, flared eave lip.
# SOLID (not a shell): the surfelizer's thin-structure rule classified a
# 2.6-cell shell's staircase surface as "thin" (long tangent run, <=3-cell
# radial+vertical cross-section) and emitted 0.55-0.75-cell ellipses, which
# leave a pinhole field on the step edges. A solid mass keeps the radial run
# long, so every surface cell gets the full round disk.
for dy in range(CONE0, CONE1 + 1):
    t = (dy - CONE0) / float(CONE1 - CONE0)
    r_o = SPIRE_R * (1.0 - t)
    rr = int(r_o) + 1
    for dx in range(-rr, rr + 1):
        for dz in range(-rr, rr + 1):
            if cil(dx, dz) <= r_o:
                band = (dy // 4) % 3
                m = (M_SHINGLE_D if band == 0 else
                     (M_SHINGLE_L if band == 1 else M_SHINGLE))
                put(dx, dy, dz, m, tex=7 if h3(dx, dy, dz, 5) > 0.25 else 0)
for dy in (CONE0 - 1, CONE0):
    for dx in range(-30, 31):
        for dz in range(-30, 31):
            r = cil(dx, dz)
            if SPIRE_R - 1.5 <= r <= SPIRE_R + 1.5:
                put(dx, dy, dz, M_SHINGLE_D, tex=7)

# finial: stone ball + iron spike + pennant
for dy in range(CONE1, CONE1 + 3):
    for dx in range(-2, 3):
        for dz in range(-2, 3):
            if cil(dx, dz) <= 1.9:
                put(dx, dy, dz, M_STONE_L)
for dy in range(CONE1 + 3, CONE1 + 10):
    put(0, dy, 0, IRON)
    put(1, dy, 0, IRON)
for t in range(1, 5):                                # green pennant, trails -x
    for dy in range(CONE1 + 5, CONE1 + 9):
        put(-t, dy, 0, M_GRASS, amp=16)

# ------------------------------------------------- 6. interior furnishings --
def barrel(dx, dy, dz):
    for ddy in range(0, 4):
        r = 2.6 if ddy in (1, 2) else 2.0
        for ddx in range(-3, 4):
            for ddz in range(-3, 4):
                if cil(ddx, ddz) <= r:
                    put(dx + ddx, dy + ddy, dz + ddz, M_OAK_D,
                        tex=T_BARK if ddy in (1, 2) else 0)


def crate(dx, dy, dz, s=4):
    for ddx in range(s):
        for ddy in range(s):
            for ddz in range(s):
                if ddx in (0, s - 1) or ddy in (0, s - 1) or ddz in (0, s - 1):
                    put(dx + ddx, dy + ddy, dz + ddz, M_OAK_L, tex=T_PLANK)


def lantern(dx, dy, dz):
    put(dx, dy, dz, M_OAK_D, tex=T_PLANK)
    put(dx, dy + 1, dz, IRON)
    put(dx, dy + 2, dz, M_EMBER)


barrel(-14, 2, 6)
barrel(-11, 2, 11)
crate(-15, 2, -8)
lantern(-18, 12, 2)
for ddx in range(-5, 6):                             # first-floor table
    for ddz in range(-3, 4):
        put(ddx, DECK1 + 5, -8 + ddz, M_OAK, tex=T_PLANK)
for (lx, lz) in ((-4, -11), (4, -11), (-4, -5), (4, -5)):
    for dy in range(DECK1 + 1, DECK1 + 5):
        put(lx, dy, lz, M_OAK_D, tex=T_BARK)
crate(10, DECK1 + 1, 6, 5)
lantern(18, DECK1 + 10, -4)
for ddx in range(-9, -1):                            # second-floor straw bed
    for ddz in range(2, 11):
        put(ddx, DECK2 + 1, ddz,
            dict(mat=FOLIAGE, rgb=[176, 152, 82], refl=30, rough=235))
for ddx in range(-9, -1):
    put(ddx, DECK2 + 1, 1, M_OAK_D, tex=T_BARK)
crate(2, DECK2 + 1, -14, 5)
lantern(17, DECK2 + 9, 6)

# ------------------------------------------------------ 7. exterior dress --
for dx in range(-30, 31):                            # moss at the foot
    for dz in range(-30, 31):
        r = cil(dx, dz)
        if R_OUT + 1.0 <= r <= R_OUT + 4.5 and h3(dx, dz, 7, 3) < 0.16:
            put(dx, 0, dz, M_MOSS, tex=T_MOSS)
            if h3(dx, dz, 7, 4) < 0.4:
                put(dx, 1, dz, M_MOSS, tex=T_MOSS)
for dx in range(-30, 31):                            # ivy on the NW face
    for dz in range(-30, 31):
        a = ang_of(dx, dz)
        r = cil(dx, dz)
        if not (R_OUT <= r <= R_OUT + 1.5 and in_ang(a, 135.0, 30.0)):
            continue
        top = 6 + int(22 * (h3(dx, dz, 11, 5) ** 1.4))
        for dy in range(0, top):
            if h3(dx, dz, dy, 6) < 0.72:
                put(dx, dy, dz, M_GRASS, amp=16)
for sgn in (-1, 1):                                  # torches by the door
    ta = DOOR_A + sgn * 26
    ddx = int(round(R_OUT * math.cos(math.radians(ta))))
    ddz = int(round(R_OUT * math.sin(math.radians(ta))))
    put(ddx, 16, ddz, M_OAK_D, tex=T_BARK)
    put(ddx, 17, ddz, IRON)
    put(ddx, 18, ddz, M_EMBER)
for dy in (1,):                                     # threshold pad in the reveal
    for dx in range(-30, 31):
        for dz in range(-30, 31):
            if cil(dx, dz) <= R_OUT + 1.5 and in_ang(ang_of(dx, dz), DOOR_A, 10.0):
                put(dx, dy, dz, M_STONE_L)

# window sills + glazing (glass panes one cell behind the reveal)
WINDOWS = [(sa, 40) for sa in (75, 195, 315)] + [(sa, 68) for sa in (30, 150, 270)]
for sa, sill in WINDOWS:
    for dx in range(-30, 31):                         # projecting stone sill
        for dz in range(-30, 31):
            a = ang_of(dx, dz)
            r = cil(dx, dz)
            if in_ang(a, sa, 11.0) and R_OUT <= r <= R_OUT + 1.5:
                put(dx, sill - 1, dz, M_STONE_L)
            if not (20.5 <= r <= 21.5):
                continue
            for dy in range(sill, sill + 19):         # pane + mullions, up the arch
                if dy <= sill + 13:
                    half = 8.4
                else:
                    t = (dy - (sill + 13)) / 5.0
                    half = 9.5 * (1.0 - 0.8 * t) - 0.5
                if half < 0.3:
                    break
                if not in_ang(a, sa, half):
                    continue
                if in_ang(a, sa, 0.8) or dy in (sill + 5, sill + 6):
                    put(dx, dy, dz, M_OAK_D, tex=T_PLANK)
                else:
                    put(dx, dy, dz, M_GLASS)

out = list(cells.values())
print(f"{len(out)} voxels")
xs = [r["x"] for r in out]; ys = [r["y"] for r in out]; zs = [r["z"] for r in out]
print(f"bounds x[{min(xs)}..{max(xs)}] y[{min(ys)}..{max(ys)}] "
      f"z[{min(zs)}..{max(zs)}]  size {(max(xs)-min(xs)+1)*0.1:.1f}x"
      f"{(max(ys)-min(ys)+1)*0.1:.1f}x{(max(zs)-min(zs)+1)*0.1:.1f} m")

payload = {"name": "hamlet_tower_v2", "voxels": out}
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
