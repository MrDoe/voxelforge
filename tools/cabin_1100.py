#!/usr/bin/env python3
"""Author the hamlet hall as an 11th-century (c.1100) English timber hall.

Replaces assets/hamlet_hall.vxw via the vf_mcp write_object tool.
Everything is emitted as explicit absolute lattice cells so the shell is
hollow (walls/roof only) - the VoxelField flood-fills the interior solid.

Period vocabulary:
  * cruck-like timber frame: dark oak posts, plate beams, diagonal braces
  * wattle-and-daub panels: pale clay between the framing
  * stone plinth the walls sit on, sunk to the local terrain height
  * open hall: no chimney - smoke vent (louver) in the roof slope
  * small unglazed window slits with sliding timber shutters
  * oak board-and-batten door in a recessed frame with a threshold step
  * shingled roof: coursing + eaves drip + gable barges + ridge cap
"""
import json
import subprocess
import sys

import numpy as np
from PIL import Image

VOXEL = 0.1
WORLD = 102.4
HALF = 0.5 * WORLD
LAT = 1024  # lattice extent

# ---------------------------------------------------------------- terrain ----
_HM = np.asarray(Image.open("assets/heightmap.png"), dtype=np.float64)
_HM = -8.0 + (_HM / 65535.0) * 32.0


def terrain(x, z):
    """Bilinear terrain height in metres, matching HeightMap::sample."""
    u = min(max(x / WORLD + 0.5, 0.0), 1.0)
    v = min(max(z / WORLD + 0.5, 0.0), 1.0)
    H, W = _HM.shape
    fx, fz = u * (W - 1), v * (H - 1)
    x0, z0 = int(np.floor(fx)), int(np.floor(fz))
    x1, z1 = min(x0 + 1, W - 1), min(z0 + 1, H - 1)
    a, b = fx - x0, fz - z0
    return float((_HM[z0, x0] * (1 - a) + _HM[z0, x1] * a) * (1 - b)
                 + (_HM[z1, x0] * (1 - a) + _HM[z1, x1] * a) * b)


def cell(x_m, y_m, z_m):
    """World metres -> lattice cell index."""
    return (int(round((x_m + HALF) / VOXEL)),
            int(round((y_m + HALF) / VOXEL)),
            int(round((z_m + HALF) / VOXEL)))


# ------------------------------------------------------------- materials ----
# mat ids: 4 rock, 5 light rock, 6 wood, 7 roof shingle, 8 foliage
ROCK, LIGHT_ROCK, WOOD, SHINGLE, FOLIAGE = 4, 5, 6, 7, 8

M_OAK_DARK = dict(mat=WOOD, rgb=[96, 62, 32], refl=70, rough=160)
M_OAK = dict(mat=WOOD, rgb=[124, 82, 44], refl=70, rough=150)
M_OAK_LIGHT = dict(mat=WOOD, rgb=[148, 104, 56], refl=70, rough=140)
M_DAUB = dict(mat=WOOD, rgb=[178, 162, 128], refl=60, rough=190)  # wattle panel
M_EARTH = dict(mat=2, rgb=[88, 62, 34], refl=55, rough=210)       # beaten floor
M_STONE = dict(mat=ROCK, rgb=[112, 106, 98], refl=95, rough=150)
M_STONE_LIGHT = dict(mat=LIGHT_ROCK, rgb=[150, 146, 138], refl=115, rough=135)
M_SHINGLE = dict(mat=SHINGLE, rgb=[96, 70, 42], refl=60, rough=170)
M_SHINGLE_DK = dict(mat=SHINGLE, rgb=[78, 56, 34], refl=60, rough=170)
M_SHINGLE_LT = dict(mat=SHINGLE, rgb=[112, 82, 50], refl=60, rough=170)
M_THATCH = dict(mat=FOLIAGE, rgb=[122, 96, 46], refl=55, rough=200)
M_IRON = dict(mat=ROCK, rgb=[58, 56, 60], refl=140, rough=90)

# per-cell texture override (atlas layer). world.json binds:
#   6 -> wood.png (planks), 7 -> shingles.png, 17 -> bark_brown.png (bark),
#   19 -> proc_thatch.png
# Framing carries the plank texture; shingles carry the shingle texture;
# the daub panels keep the palette colour (no slot) so they read as clay.
T_PLANK = dict(tex=6)
T_BARK = dict(tex=17)
T_NONE = {}


def rec(x, y, z, m, **over):
    r = {"x": x, "y": y, "z": z, "mat": m["mat"],
         "r": m["rgb"][0], "g": m["rgb"][1], "b": m["rgb"][2],
         "refl": m["refl"], "rough": m["rough"]}
    if m is M_DAUB:
        r.update(T_NONE)
    elif m["mat"] == WOOD:
        r.update(T_PLANK if not over.pop("bark", False) else T_BARK)
    elif m["mat"] == SHINGLE:
        r.update(T_PLANK if over.pop("thatch", False) else {})
    r.update(over)
    return r


# ----------------------------------------------------------------- build ----
def build():
    recs = {}
    add = lambda r: recs.__setitem__((r["x"], r["y"], r["z"]), r)

    # footprint: long axis along X, ridge parallel to X. Camera (house shot)
    # looks from low X/Z toward the hall, so the entry goes on the -Z wall.
    x0, x1 = 6.2, 15.4          # 9.2 m long
    z0, z1 = 11.9, 16.6         # 4.7 m wide
    yc = 0.0                    # nominal ground level of the site
    floor_h = 0.30              # top of stone plinth = interior floor
    wall_top = 2.90             # eaves height above floor
    eave_drop = 0.35            # roof overhang beyond the wall face
    pitch = 0.62                # roof rise per metre of run (33 deg)

    cx0, cy0, cz0 = cell(x0, 0, z0)
    cx1, cy1, cz1 = cell(x1, 0, z1)
    cfloor = int(round((floor_h + HALF) / VOXEL))
    cwall = int(round((wall_top + HALF) / VOXEL))

    # local terrain min across the footprint -> plinth base
    xs = np.arange(x0, x1 + 0.05, 0.1)
    zs = np.arange(z0, z1 + 0.05, 0.1)
    tmin = 1e9
    for zx in zs:
        for xx in xs:
            tmin = min(tmin, terrain(float(xx), float(zx)))
    base_h = min(tmin, -0.2) - 0.15        # plinth sole, below local ground
    cbase = int(round((base_h + HALF) / VOXEL))
    print(f"terrain min {tmin:.2f} -> plinth base {base_h:.2f} "
          f"(cells {cbase}..{cfloor})")

    def stone_plinth():
        # dressed-stone plinth block: outer face is coursed stone, top is the
        # floor, and the interior is packed with earth + gravel up to the
        # floor so nothing floats and nothing is hollow underfoot.
        for cxx in range(cx0 - 1, cx1 + 2):
            for czz in range(cz0 - 1, cz1 + 2):
                wx = -HALF + (cxx + 0.5) * VOXEL
                wz = -HALF + (czz + 0.5) * VOXEL
                if not (x0 - 0.25 <= wx <= x1 + 0.25 and z0 - 0.25 <= wz <= z1 + 0.25):
                    continue
                # ground height at this column: the sole sinks 0.15 m below it
                gh = terrain(wx, wz)
                cb = int(round((min(gh, base_h) + HALF) / VOXEL))
                for cyy in range(cb, cfloor + 1):
                    wy = -HALF + (cyy + 0.5) * VOXEL
                    on_face = (abs(wx - (x0 - 0.15)) < 0.06 or abs(wx - (x1 + 0.15)) < 0.06
                               or abs(wz - (z0 - 0.15)) < 0.06 or abs(wz - (z1 + 0.15)) < 0.06)
                    on_top = abs(wy - floor_h) < 0.06
                    if on_face:
                        m = M_STONE_LIGHT if (cyy - cb) % 4 == 0 else M_STONE
                        add(rec(cxx, cyy, czz, m))
                    elif on_top:
                        add(rec(cxx, cyy, czz, M_STONE_LIGHT))
                    elif wy < floor_h - 0.10:
                        # interior pack: beaten earth over rubble
                        m = M_STONE if (cyy - cb) % 5 == 0 else M_EARTH
                        add(rec(cxx, cyy, czz, m))

    stone_plinth()

    # floorboards spanning the interior, resting on the pack
    for cxx in range(cx0, cx1 + 1):
        for czz in range(cz0, cz1 + 1):
            wx = -HALF + (cxx + 0.5) * VOXEL
            wz = -HALF + (czz + 0.5) * VOXEL
            if x0 + 0.05 <= wx <= x1 - 0.05 and z0 + 0.05 <= wz <= z1 - 0.05:
                cyy = cfloor + 1
                m = M_OAK if (cxx + czz) % 2 == 0 else M_OAK_LIGHT
                add(rec(cxx, cyy, czz, m))

    # ---- timber frame -------------------------------------------------
    # bay spacing: posts every 1.5 m along X, one ring of framing.
    post_w = 0.20
    bay = 1.5
    x_posts = [x0 + i * bay for i in range(int(round((x1 - x0) / bay)) + 1)]
    if x_posts[-1] < x1 - 0.01:
        x_posts.append(x1)

    def add_box(xa, xb, ya, yb, za, zb, m, **over):
        cxa, cya, cza = cell(xa, ya, za)
        cxb, cyb, czb = cell(xb, yb, zb)
        for cxx in range(cxa, cxb + 1):
            for cyy in range(cya, cyb + 1):
                for czz in range(cza, czb + 1):
                    add(rec(cxx, cyy, czz, m, **over))

    # wall line Z faces (front z0 and back z1) and X gables: shell of posts
    z_faces = [z0, z1]
    x_faces = [x0, x1]
    wall_inset = 0.20            # framing stands on the plinth edge

    # corner + wall posts (front and back walls carry the plate)
    for xp in x_posts:
        for zf in z_faces:
            add_box(xp - post_w / 2, xp + post_w / 2,
                    floor_h, floor_h + wall_top - 0.2,
                    zf - post_w / 2, zf + post_w / 2, M_OAK_DARK, bark=True)
    # gable-end posts (slightly inside so the roof barge covers them)
    for xf in x_faces:
        add_box(xf - post_w / 2, xf + post_w / 2,
                floor_h, floor_h + wall_top - 0.2,
                z0 + wall_inset, z0 + wall_inset + post_w, M_OAK_DARK, bark=True)
        add_box(xf - post_w / 2, xf + post_w / 2,
                floor_h, floor_h + wall_top - 0.2,
                z1 - wall_inset - post_w, z1 - wall_inset, M_OAK_DARK, bark=True)

    # sill beam (bottom rail) and wall plate (top rail) on both long walls
    for zf in z_faces:
        add_box(x0, x1, floor_h, floor_h + 0.18,
                zf - 0.10, zf + 0.10, M_OAK, bark=True)
        add_box(x0, x1, floor_h + wall_top - 0.22, floor_h + wall_top,
                zf - 0.12, zf + 0.12, M_OAK, bark=True)
    # mid rail
    for zf in z_faces:
        add_box(x0, x1, floor_h + 1.35, floor_h + 1.50,
                zf - 0.08, zf + 0.08, M_OAK_DARK, bark=True)
    # gable rails
    for xf in x_faces:
        add_box(xf - 0.10, xf + 0.10, floor_h, floor_h + wall_top - 0.2,
                z0 + wall_inset, z1 - wall_inset, M_OAK_DARK, bark=True)

    # diagonal braces at each post on the long walls (brace against the sill)
    for xp in x_posts[::2]:
        for zf in z_faces:
            sgn = 1 if zf > 0 else -1
            for t in np.arange(0.0, 1.05, 0.1):
                xa = xp - 0.9 + 0.9 * t
                ya = floor_h + 0.18 + 1.0 * t
                cxa, cya, cza = cell(xa, ya, zf)
                for dxx in (-1, 0):
                    for dyy in (0,):
                        add(rec(cxa + dxx, cya, cza, M_OAK_DARK, bark=True))

    # ---- wattle-and-daub panels ---------------------------------------
    panel_in = 0.06               # panel sits just inside the outer face
    for zf, sgn in ((z0, 1), (z1, 1)):
        for i in range(len(x_posts) - 1):
            xa, xb = x_posts[i] + post_w / 2 + 0.02, x_posts[i + 1] - post_w / 2 - 0.02
            if xb - xa < 0.2:
                continue
            # two panel leaves with a rail break at mid height
            for y0p, y1p in ((floor_h + 0.22, floor_h + 1.32),
                             (floor_h + 1.55, floor_h + wall_top - 0.26)):
                add_box(xa, xb, y0p, y1p,
                        zf - panel_in - 0.08, zf - panel_in, M_DAUB)

    # gable-end infill (below the triangle) with a small window slit
    for xf in x_faces:
        add_box(xf - 0.08, xf + 0.08, floor_h + 0.22, floor_h + 1.32,
                z0 + wall_inset + 0.22, z1 - wall_inset - 0.22, M_DAUB)
        add_box(xf - 0.08, xf + 0.08, floor_h + 1.55, floor_h + wall_top - 0.26,
                z0 + wall_inset + 0.22, z1 - wall_inset - 0.22, M_DAUB)

    # ---- windows: slits with a frame, sill and open shutter ----------
    # front wall (-Z) faces the camera; put 3 windows + the door there.
    win_w, win_h = 0.55, 0.85
    sill_h = floor_h + 1.05
    front_win_x = [x0 + 1.55, x0 + 4.4, x0 + 7.6]
    back_win_x = [x0 + 2.2, x0 + 6.4]

    def window(xc, zf, shutter_side):
        za = zf - panel_in - 0.20          # outer face of the wall
        zb = zf - panel_in - 0.02
        ya, yb = sill_h, sill_h + win_h
        # frame (dark oak lining the opening)
        add_box(xc - win_w / 2 - 0.08, xc + win_w / 2 + 0.08, ya - 0.08, yb + 0.06,
                za - 0.02, za + 0.04, M_OAK_DARK, bark=True)
        add_box(xc - win_w / 2 - 0.08, xc + win_w / 2 + 0.08, yb + 0.06, yb + 0.16,
                za - 0.02, zb, M_OAK_DARK, bark=True)
        add_box(xc - win_w / 2 - 0.08, xc - win_w / 2 + 0.02, ya, yb,
                za - 0.02, zb, M_OAK_DARK, bark=True)
        add_box(xc + win_w / 2 - 0.02, xc + win_w / 2 + 0.08, ya, yb,
                za - 0.02, zb, M_OAK_DARK, bark=True)
        # sill
        add_box(xc - win_w / 2 - 0.12, xc + win_w / 2 + 0.12, ya - 0.10, ya,
                za - 0.06, zb + 0.04, M_OAK_DARK, bark=True)
        # open shutter: a board angled out from the wall on one side
        sx = xc - win_w / 2 - 0.10 if shutter_side < 0 else xc + win_w / 2 + 0.10
        for t in np.arange(0.0, 1.01, 0.1):
            xx = sx + shutter_side * 0.28 * t
            zz = za - 0.28 * t
            cxx, cyy, czz = cell(xx, sill_h + win_h / 2, zz)
            for dy in range(-4, 5):
                add(rec(cxx, cyy + dy, czz, M_OAK_DARK, bark=True))
            # top/bottom rail of the shutter
            add(rec(cxx, cyy - 5, czz, M_OAK_DARK, bark=True))
            add(rec(cxx, cyy + 5, czz, M_OAK_DARK, bark=True))

    for i, xc in enumerate(front_win_x):
        window(xc, z0, -1 if i % 2 == 0 else 1)
    for i, xc in enumerate(back_win_x):
        window(xc, z1, 1 if i % 2 == 0 else -1)

    # gable slit (high, small)
    for xf in x_faces:
        xc = xf
        ya, yb = floor_h + wall_top - 0.75, floor_h + wall_top - 0.30
        add_box(xc - 0.04, xc + 0.04, ya, yb,
                z0 + 0.9, z1 - 0.9, M_OAK_DARK, bark=True)
        # cut a real hole: remove the daub behind the slit
        for k in list(recs):
            if abs(k[0] - cell(xc, 0, 0)[0]) <= 0 and ya - 0.15 < -HALF + (k[1] + 0.5) * VOXEL < yb + 0.15:
                if z0 + 0.8 < -HALF + (k[2] + 0.5) * VOXEL < z1 - 0.8 and k[1] > cfloor:
                    recs.pop(k, None)

    # ---- entry: board-and-batten door in a recessed frame -------------
    door_x = x0 + 5.6               # centred-ish on the front wall
    door_w, door_h = 1.10, 2.10
    dy0, dy1 = floor_h, floor_h + door_h

    # reveal: clear the front-wall shell where the door sits
    for k in list(recs):
        wx = -HALF + (k[0] + 0.5) * VOXEL
        wy = -HALF + (k[1] + 0.5) * VOXEL
        wz = -HALF + (k[2] + 0.5) * VOXEL
        if (door_x - door_w / 2 - 0.35 < wx < door_x + door_w / 2 + 0.35
                and dy0 - 0.35 < wy < dy1 + 0.35
                and z0 - 0.35 < wz < z0 + 0.30):
            recs.pop(k, None)

    # door frame: jambs + lintel, proud of the wall
    for jx in (door_x - door_w / 2 - 0.10, door_x + door_w / 2 + 0.02):
        add_box(jx, jx + 0.12, dy0 - 0.06, dy1 + 0.16,
                z0 - 0.22, z0 + 0.06, M_OAK_DARK, bark=True)
    add_box(door_x - door_w / 2 - 0.10, door_x + door_w / 2 + 0.12,
            dy1 + 0.04, dy1 + 0.20, z0 - 0.22, z0 + 0.06, M_OAK_DARK, bark=True)
    # threshold
    add_box(door_x - door_w / 2 - 0.18, door_x + door_w / 2 + 0.18,
            floor_h - 0.08, floor_h, z0 - 0.20, z0 + 0.10, M_STONE_LIGHT)
    # step down to the ground
    add_box(door_x - door_w / 2 - 0.30, door_x + door_w / 2 + 0.30,
            floor_h - 0.22, floor_h - 0.08, z0 - 0.42, z0 - 0.18, M_STONE_LIGHT)

    # the door itself: board-and-batten, slightly open (angled out)
    swing = 0.16
    for t in np.arange(0.0, 1.001, 0.05):
        # left leaf
        xx = door_x - door_w / 2 + 0.06 + (door_w / 2 - 0.10) * t
        zz = z0 - 0.05 - swing * t
        cxx, cyy, czz = cell(xx, dy0 + door_h / 2, zz)
        for dy in range(0, int(door_h / VOXEL) + 1):
            add(rec(cxx, cyy - int(door_h / VOXEL / 2) + dy, czz, M_OAK, bark=True))
        # battens
        add(rec(cxx, cyy - 3, czz, M_OAK_DARK, bark=True))
        add(rec(cxx, cyy + 3, czz, M_OAK_DARK, bark=True))
    # right leaf closed in the plane
    add_box(door_x + 0.02, door_x + door_w / 2 - 0.02, dy0 + 0.04, dy1 - 0.04,
            z0 - 0.06, z0 - 0.01, M_OAK, bark=True)
    add_box(door_x + 0.02, door_x + door_w / 2 - 0.02, dy0 + 0.55, dy0 + 0.70,
            z0 - 0.07, z0 + 0.01, M_OAK_DARK, bark=True)
    add_box(door_x + 0.02, door_x + door_w / 2 - 0.02, dy1 - 0.75, dy1 - 0.60,
            z0 - 0.07, z0 + 0.01, M_OAK_DARK, bark=True)
    # iron strap hinges
    for hy in (dy0 + 0.25, dy1 - 0.30):
        add_box(door_x + 0.00, door_x + 0.10, hy, hy + 0.16,
                z0 - 0.09, z0 - 0.02, M_IRON)

    # ---- roof ----------------------------------------------------------
    # gabled roof, ridge along X at the centre of Z.
    ridge_y = floor_h + wall_top + 0.5 * (z1 - z0) * pitch
    eave_y = floor_h + wall_top - 0.10
    z_eave0 = z0 - eave_drop
    z_eave1 = z1 + eave_drop
    x_g0 = x0 - eave_drop
    x_g1 = x1 + eave_drop

    # common rafters every 0.6 m, projecting past the eaves (rafter tails)
    rafter_w = 0.10
    rafter_xs = np.arange(x0 + 0.3, x1 - 0.2, 0.6)
    for rx in rafter_xs:
        for side in (0, 1):
            zc = (z0 + z1) / 2
            # rafter line from eave to ridge
            for t in np.arange(0.0, 1.001, 0.05):
                zz = zc + (-(z1 - z0) / 2 - eave_drop) * (1 - t) + 0 * t if side == 0 \
                    else zc + ((z1 - z0) / 2 + eave_drop) * (1 - t)
                zz = (z0 - eave_drop) + (zc - (z0 - eave_drop)) * t if side == 0 \
                    else (z1 + eave_drop) - ((z1 + eave_drop) - zc) * t
                yy = eave_y + (ridge_y - eave_y) * t
                cxx, cyy, czz = cell(rx, yy, zz)
                add(rec(cxx, cyy, czz, M_OAK_DARK, bark=True))
                add(rec(cxx + 1, cyy, czz, M_OAK_DARK, bark=True))

    # ridge beam
    add_box(x0 - 0.05, x1 + 0.05, ridge_y - 0.10, ridge_y + 0.02,
            (z0 + z1) / 2 - 0.09, (z0 + z1) / 2 + 0.09, M_OAK_DARK, bark=True)
    # purlins (mid-slope rails) X-aligned
    for frac in (0.35, 0.7):
        for side in (-1, 1):
            zc = (z0 + z1) / 2 + side * (z1 - z0) / 2 * frac
            yy = eave_y + (ridge_y - eave_y) * (1 - frac)
            add_box(x0 - 0.02, x1 + 0.02, yy - 0.05, yy + 0.05,
                    zc - 0.07, zc + 0.07, M_OAK, bark=True)

    # shingle skin: two sloped layers, coursed
    shingle_thick = 0.10
    course_h = 0.34                     # vertical band per shingle course
    for side in (-1, 1):
        z_outer = (z0 - eave_drop) if side < 0 else (z1 + eave_drop)
        z_inner = (z0 + z1) / 2
        n_steps = int(round(abs(z_outer - z_inner) / 0.1))
        for s in range(n_steps + 1):
            t = s / max(n_steps, 1)
            zz = z_inner + side * (z_outer - z_inner) * 0 - (z_outer - z_inner) * t * side
            zz = z_inner + (z_outer - z_inner) * t * side
            # top surface of the shingle layer at this Z
            yy_top = eave_y + (ridge_y - eave_y) * t
            yy_bot = yy_top - shingle_thick
            czz = int(round((zz + HALF) / VOXEL))
            # vertical rise of this course band
            band = int((yy_top - eave_y) / course_h)
            m = (M_SHINGLE_DK if band % 3 == 0 else
                 (M_SHINGLE_LT if band % 3 == 1 else M_SHINGLE))
            for cxx in range(int(round((x_g0 + HALF) / VOXEL)),
                             int(round((x_g1 + HALF) / VOXEL)) + 1):
                for cyy in range(int(round((yy_bot + HALF) / VOXEL)),
                                 int(round((yy_top + HALF) / VOXEL)) + 1):
                    add(rec(cxx, cyy, czz, m))

    # eaves drip: front lip of shingles lower + forward
    for side in (-1, 1):
        ze = (z0 - eave_drop - 0.10) if side < 0 else (z1 + eave_drop + 0.10)
        add_box(x_g0, x_g1, eave_y - 0.16, eave_y - 0.02, ze - 0.04, ze + 0.04,
                M_SHINGLE_DK)
    # ridge cap
    add_box(x_g0 - 0.05, x_g1 + 0.05, ridge_y + 0.02, ridge_y + 0.14,
            (z0 + z1) / 2 - 0.16, (z0 + z1) / 2 + 0.16, M_SHINGLE_DK)

    # ---- gable triangle: timbering + daub -----------------------------
    for xf in x_faces:
        cxf = int(round((xf + HALF) / VOXEL))
        # collar tie
        ct_y = ridge_y - (ridge_y - eave_y) * 0.35
        half_span = (z1 - z0) / 2
        for t in np.arange(0.0, 1.001, 0.1):
            zz = (z0 + z1) / 2 - half_span * 0.9 * t
            yy = eave_y + (ridge_y - eave_y) * (0.55 + 0.45 * t)
            cxx, cyy, czz = cell(xf, yy, zz)
            add(rec(cxf, cyy, czz, M_OAK_DARK, bark=True))
        # vertical king post under the ridge
        add_box(xf - 0.06, xf + 0.06, eave_y + 0.3, ridge_y - 0.06,
                (z0 + z1) / 2 - 0.06, (z0 + z1) / 2 + 0.06, M_OAK_DARK, bark=True)
        # studs in the triangle
        for frac in (0.3, 0.55, 0.8):
            zz = (z0 + z1) / 2 - half_span * frac
            yy = eave_y + (ridge_y - eave_y) * (1 - frac) * 0.92
            add_box(xf - 0.05, xf + 0.05, yy, yy + 0.6,
                    zz - 0.04, zz + 0.04, M_OAK_DARK, bark=True)
        # daub infill in the triangle
        for t in np.arange(0.05, 1.0, 0.1):
            zz = (z0 + z1) / 2 - half_span * t
            yy = eave_y + (ridge_y - eave_y) * (1 - t) * 0.95
            add_box(xf - 0.05, xf + 0.05, yy, yy + 0.35,
                    zz - 0.05, zz + 0.05, M_DAUB)

    # ---- smoke louver over the open hearth (no chimney in 1100) -------
    lv_z = (z0 + z1) / 2 + 0.9
    lv_y = ridge_y - 0.35
    add_box(x0 + 3.4, x0 + 4.2, lv_y, lv_y + 0.5, lv_z - 0.18, lv_z + 0.18,
            M_OAK_DARK, bark=True)
    for s in range(5):
        yy = lv_y + 0.05 + s * 0.09
        add_box(x0 + 3.45, x0 + 4.15, yy, yy + 0.04,
                lv_z - 0.16, lv_z + 0.16, M_OAK_DARK, bark=True)

    # ---- interior: hearth + trestle table (glimpsed through the door) --
    add_box(x0 + 3.6, x0 + 4.0, floor_h, floor_h + 0.35, (z0 + z1) / 2 - 0.25,
            (z0 + z1) / 2 + 0.25, M_STONE)
    add_box(x0 + 5.2, x0 + 6.0, floor_h + 0.72, floor_h + 0.78,
            (z0 + z1) / 2 - 0.55, (z0 + z1) / 2 + 0.55, M_OAK)
    for tx in (x0 + 5.3, x0 + 5.9):
        for tz in ((z0 + z1) / 2 - 0.5, (z0 + z1) / 2 + 0.5):
            add_box(tx, tx + 0.07, floor_h, floor_h + 0.72, tz, tz + 0.07, M_OAK)

    out = list(recs.values())
    return out


def main():
    recs = build()
    print(f"{len(recs)} voxels")
    # sanity: bounds
    xs = [r["x"] for r in recs]
    ys = [r["y"] for r in recs]
    zs = [r["z"] for r in recs]
    print(f"bounds x[{min(xs)}..{max(xs)}] y[{min(ys)}..{max(ys)}] "
          f"z[{min(zs)}..{max(zs)}]  size {(max(xs)-min(xs)+1)*0.1:.1f}x"
          f"{(max(ys)-min(ys)+1)*0.1:.1f}x{(max(zs)-min(zs)+1)*0.1:.1f} m")

    vox = []
    for r in recs:
        v = {"x": r["x"], "y": r["y"], "z": r["z"], "mat": r["mat"],
             "r": r["r"], "g": r["g"], "b": r["b"],
             "refl": r["refl"], "rough": r["rough"]}
        if r.get("tex"):
            v["tex"] = r["tex"]
        vox.append(v)
    payload = {"name": "hamlet_hall", "voxels": vox}
    req = ("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
           "\"params\":{\"name\":\"write_object\",\"arguments\":"
           + json.dumps(payload, separators=(",", ":")) + "}}\n")
    print("payload bytes:", len(req))
    p = subprocess.run(["./build/vf_mcp"], input=req, capture_output=True,
                       text=True, timeout=600)
    for line in p.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            d = json.loads(line)
            txt = d.get("result", {}).get("content", [{}])[0].get("text", "")
            print("mcp:", txt[:400])
        except Exception:
            print("raw:", line[:300])
    if p.stderr:
        print("stderr:", p.stderr[:400])


if __name__ == "__main__":
    main()
