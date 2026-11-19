#!/usr/bin/env python3
"""Cut the open doorways + author the building interiors of LighthouseIsle.

Runs after isle_meshes.sh imported the shell-mode house/barn/windmill. For
each mesh building:

  1. measure the real placed bounds via vf_mcp inspect_layer (bounds come
     from the written layer, not the source mesh);
  2. cut a doorway through the SOUTH skin with isle_opendoor (doors cannot
     be articulated yet, so every door is modeled OPEN — user directive
     2026-11-19). The opening is what keeps a hollow interior from being
     flood-filled solid by the field builder;
  3. write isle_interiors: a stone FOOTING under every building (v3 rule:
     the landscape bakes no pads — each building brings its own base), house
     floor rug, hearth + ember fire, table, stools, bed, chest, shelf,
     hanging lamp; barn hay mounds + barrels + crates + lantern; windmill
     sacks, wallot gear, ladder. Furniture is placed from the measured
     interior center.

Also owns the manifest "lights" key: authored lights fill the UBO before
derived ones, which matters because the water volume (mat 9, emissive) floods
the derived budget on an open-sea world.
"""
import json
import math
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from isle_nature import Layer, cell, terrain  # Layer + lattice + heightmap

ENV = dict(os.environ, VF_SCENERY="LighthouseIsle")
BIN = os.path.join(ROOT, "build", "vf_mcp")
SCENERY = os.path.join(ROOT, "scenery", "LighthouseIsle")
OPEN_TOOL = os.path.join(SCENERY, "bin", "isle_opendoor")

# palette ids (src/voxel/common.hpp)
SOIL, ROCK, LROCK, WOOD, ROOF, FOLIAGE = 2, 4, 5, 6, 7, 8
EMBER, WHITEHOT, SNOW, THATCH = 10, 10, 16, 19   # lamps ride mat 10: the
# emissive lookup keys on the CELL MATERIAL (9-15 glow) and slot 15 has the
# gravel photo bound to it, so mat-15 lamps would glow gravel-white
WALL_T = 0.35   # mesh walls at fit 6-8 m land near a lattice skin < 0.35 m


def mcp(tool, args):
    req = ('{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"'
           + tool + '","arguments":' + json.dumps(args) + "}}\n")
    p = subprocess.run([BIN], input=req, capture_output=True, text=True,
                       timeout=900, cwd=ROOT, env=ENV)
    for line in p.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            d = json.loads(line)
            return d.get("result", {}).get("content", [{}])[0].get("text", "")
        except Exception:
            pass
    return ""


def bounds_of(layer):
    txt = mcp("inspect_layer", {"layer": layer})
    m = re.search(r"x\[(\d+)\.\.(\d+)\] y\[(\d+)\.\.(\d+)\] z\[(\d+)\.\.(\d+)\]",
                  txt)
    if not m:
        return None
    a = [int(v) for v in m.groups()]
    out = {}
    for k, i in (("x", 0), ("y", 2), ("z", 4)):
        out[k] = (a[i] * 0.1 - 51.2, a[i + 1] * 0.1 - 51.2)
    return out


def cut_door(layer, cx, y_floor, z_south, half_w, height):
    """open a doorway through the south skin. The box reaches 1.8 m INTO
    the footprint: the import --rot-y turns the wall, so the wall plane at
    the AABB centre sits up to halfwidth*sin(rot) deeper than the bounds'
    min-z edge (the v1 0.8 m window missed it: removed 0 of 33160)."""
    box = (f"{cx - half_w:.2f},{y_floor + 0.03:.2f},{z_south - 0.30:.2f},"
           f"{cx + half_w:.2f},{y_floor + height:.2f},{z_south + 1.80:.2f}")
    r = subprocess.run([OPEN_TOOL, os.path.join(SCENERY, layer + ".vxw"),
                        "--box", box], capture_output=True, text=True, env=ENV)
    print(r.stdout.strip() or r.stderr.strip())


def box_fill(L, x, y, z, w, h, dp, mat, rgb, s):
    c = cell(x, y, z)
    hw, hd = int(w * 10), int(dp * 10)
    for dx in range(-hw, hw + 1):
        for dy in range(0, int(h * 10) + 1):
            for dz in range(-hd, hd + 1):
                L.put(c[0] + dx, c[1] + dy, c[2] + dz, mat, rgb, amp=9, s=s)


def footing(L, b, fy, s):
    """Stone footing under the whole footprint, down past the LOWEST terrain
    corner: since v3 the landscape bakes no pads, so every building brings
    its own base in its own layer file (user rule 2026-11-19)."""
    x0, x1 = b["x"][0] - 0.28, b["x"][1] + 0.28
    z0, z1 = b["z"][0] - 0.28, b["z"][1] + 0.28
    gmin = min(terrain(x, z) for x in (x0, x1) for z in (z0, z1))
    ybot = min(gmin, fy - 0.3) - 0.18
    box_fill(L, (x0 + x1) / 2, ybot, (z0 + z1) / 2, (x1 - x0) / 2,
             fy - ybot + 0.06, (z1 - z0) / 2, ROCK, (118, 114, 108), s)


def main():
    L = Layer("isle_interiors")
    pts = {}   # measured light anchors, filled per building below

    # ---------------- house ----------------------------------------------
    b = bounds_of("isle_house")
    if b:
        cx = (b["x"][0] + b["x"][1]) / 2.0
        cz = (b["z"][0] + b["z"][1]) / 2.0
        fy = b["y"][0] + 0.12
        footing(L, b, fy, 9)
        cut_door("isle_house", cx, b["y"][0], b["z"][0], 0.6, 2.1)
        ix0, ix1 = b["x"][0] + 0.5, b["x"][1] - 0.5
        iz0, iz1 = b["z"][0] + 0.5, b["z"][1] - 0.5
        pts["house"] = (cx, cz, fy, ix0, iz0, iz1)
        # rug at the door
        box_fill(L, cx, fy + 0.02, iz0 + 0.8, 0.7, 0.03, 0.5, ROOF,
                 (96, 52, 44), 11)
        # hearth on the north wall, fire (mat 10 ember -> derived light)
        box_fill(L, cx, fy, iz1 - 0.5, 0.9, 0.5, 0.4, ROCK, (92, 90, 86), 12)
        box_fill(L, cx, fy + 0.12, iz1 - 0.45, 0.45, 0.3, 0.2, EMBER,
                 (232, 108, 36), 14)
        c = cell(cx, fy, iz1 - 0.55)
        for dy in range(5, 26):               # chimney breast to the roof
            for dx in range(-6, 7):
                L.put(c[0] + dx, c[1] + dy, c[2], ROCK, (92, 90, 86),
                      amp=10, s=15 + dy)
        # table at the center (top + 4 legs), two stools
        c = cell(cx - 0.2, fy, cz)
        for dx in (-4, 4):
            for dz in (-4, 4):
                for dy in range(0, 7):
                    L.put(c[0] + dx, c[1] + dy, c[2] + dz, WOOD, (96, 72, 48),
                          s=20)
        for dx in range(-5, 6):
            for dz in range(-5, 6):
                L.put(c[0] + dx, c[1] + 7, c[2] + dz, WOOD, (110, 84, 56),
                      amp=8, s=22)
        for sx in (-10, 10):
            L.blob(c[0] + sx, c[1] + 3, c[2], 2.0, WOOD, (90, 66, 44),
                   squash=0.7, s=23)
        # bed east wall (frame + white linen + pillow), chest by the west
        c = cell(ix1 - 0.9, fy, cz + 0.3)
        for dz in range(-10, 11):
            for dx in range(-4, 5):
                L.put(c[0] + dx, c[1] + 2, c[2] + dz, WOOD, (92, 68, 44), s=24)
                if abs(dx) <= 3:
                    L.put(c[0] + dx, c[1] + 3, c[2] + dz, SNOW, (226, 222, 210),
                          amp=6, s=25)
        L.blob(c[0], c[1] + 5, c[2] - 8, 2.8, THATCH, (150, 128, 84),
               squash=0.5, s=26)              # pillow
        box_fill(L, ix0 + 0.8, fy, cz - 0.9, 0.4, 0.5, 0.3, WOOD, (84, 62, 40), 27)
        # shelf on the west wall + two clay pots + hanging lamp (mat 15)
        c = cell(ix0 + 0.3, fy + 1.15, cz)
        for dz in range(-8, 9):
            L.put(c[0], c[1], c[2] + dz, WOOD, (100, 76, 50), s=28)
        for pz in (-6, 3, 7):
            L.blob(c[0] + 2, c[1] + 2, c[2] + pz, 1.5, SOIL, (98, 68, 46),
                   squash=0.85, s=29)
        L.put(c[0] + 5, c[1] + 5, c[2], WHITEHOT, (255, 242, 222), refl=40,
              rough=150, s=30)

    # ---------------- barn -------------------------------------------------
    b = bounds_of("isle_barn")
    if b:
        cx = (b["x"][0] + b["x"][1]) / 2.0
        fy = b["y"][0] + 0.10
        cut_door("isle_barn", cx, b["y"][0], b["z"][0], 1.0, 2.4)
        footing(L, b, fy, 40)
        pts["barn"] = (b["x"][0], b["z"][0], fy)
        c = cell(b["x"][0] + 1.5, fy, b["z"][1] - 1.7)
        L.blob(c[0], c[1] + 4, c[2], 6.5, THATCH, (168, 140, 86), squash=0.75,
               amp=14, s=40)
        L.blob(c[0] + 10, c[1] + 3, c[2] - 5, 5.0, THATCH, (160, 132, 82),
               squash=0.8, amp=14, s=41)
        for k, (bx, bz) in enumerate([(b["x"][1] - 1.4, b["z"][1] - 1.3),
                                      (b["x"][1] - 1.3, b["z"][1] - 2.9)]):
            c = cell(bx, fy, bz)
            for dy in range(0, 9):
                for dx in range(-3, 4):
                    for dz in range(-3, 4):
                        if dx * dx + dz * dz <= 9:
                            L.put(c[0] + dx, c[1] + dy, c[2] + dz, WOOD,
                                  (88, 64, 42), amp=10, s=42 + dy)
        c = cell(b["x"][0] + 1.3, fy, b["z"][0] + 1.6)
        for dy in range(0, 6):
            for dx in range(-3, 4):
                for dz in range(-3, 4):
                    L.put(c[0] + dx, c[1] + dy, c[2] + dz, WOOD, (100, 76, 50),
                          amp=9, s=50)
        L.put(c[0], c[1] + 9, c[2], WHITEHOT, (255, 240, 218), refl=40,
              rough=150, s=51)

    # ---------------- windmill ---------------------------------------------
    b = bounds_of("isle_windmill")
    if b:
        cx = (b["x"][0] + b["x"][1]) / 2.0
        cz = (b["z"][0] + b["z"][1]) / 2.0
        fy = b["y"][0] + 0.10
        cut_door("isle_windmill", cx, b["y"][0], b["z"][0], 0.5, 1.9)
        footing(L, b, fy, 60)
        pts["windmill"] = (cx, cz, fy)
        for k, (sx, sz) in enumerate([(cx - 1.0, cz + 0.8), (cx + 0.9, cz + 1.0),
                                      (cx + 0.3, cz - 1.0)]):
            c = cell(sx, fy, sz)
            L.blob(c[0], c[1] + 2, c[2], 3.0, THATCH, (176, 158, 116),
                   squash=0.8, amp=10, s=60 + k)   # flour sacks
        c = cell(cx, fy + 2.2, cz)
        for dx in range(-6, 7):                     # wallot gear ring
            for dz in range(-6, 7):
                dd = math.hypot(dx, dz)
                if 4.5 < dd <= 6.2:
                    L.put(c[0] + dx, c[1], c[2] + dz, WOOD, (86, 64, 40), s=64)
        for dy in range(-14, 0):                    # upright axle below
            L.put(c[0], c[1] + dy, c[2], WOOD, (78, 58, 38), s=65)
        for dy in range(0, 26):                     # ladder to the loft
            L.put(c[0] - 9, c[1] + dy, c[2] + 6, WOOD, (92, 70, 46), s=66)
            if dy % 5 == 0:
                L.put(c[0] - 8, c[1] + dy, c[2] + 6, WOOD, (92, 70, 46), s=67)

    L.write()

    # ---------------- authored lights (manifest "lights" key) -------------
    # The emissive-DERIVED path is unusable on an open-sea scenery: the
    # water-volume mark gives every submerged air cell mat 9 (lava), which is
    # EMISSIVE, so the ocean floods the 192-cluster budget and starves real
    # lamps (measured: 1996 clusters, 188 slots). Authored lights fill the
    # UBO FIRST, so they light no matter how noisy the derived queue is.
    # This list is authoritative: the key is rewritten deduped (the MCP
    # add_light double-appends, seen: the cave pair landed twice).
    with open(os.path.join(SCENERY, "world.json"), encoding="utf-8") as f:
        man = json.load(f)
    # positions are DERIVED from the measured bounds — no hardcoded coords:
    # since v3 the buildings sit on natural ground and can shift with it.
    lights = []
    lb = bounds_of("isle_lighthouse")
    if lb:   # beacon: lantern room centre (top bounds = spire tip, -3 m)
        lights.append(dict(pos=[round((lb["x"][0] + lb["x"][1]) / 2, 2),
                                round(lb["y"][1] - 3.0, 2),
                                round((lb["z"][0] + lb["z"][1]) / 2, 2)],
                           color=[1.0, 0.95, 0.85], radius=12.0, intensity=7.0))
    jb = bounds_of("isle_jetty")
    if jb:   # jetty lamp: end post, sea side (min z), just below the top
        lights.append(dict(pos=[round((jb["x"][0] + jb["x"][1]) / 2 + 0.4, 2),
                                round(jb["y"][1] - 0.3, 2),
                                round(jb["z"][0] + 0.6, 2)],
                           color=[1.0, 0.7, 0.4], radius=4.0, intensity=1.6))
    if "house" in pts:
        cx, cz, fy, ix0, iz0, iz1 = pts["house"]
        lights.append(dict(pos=[round(cx, 2), round(fy + 0.5, 2),
                                round(iz1 - 0.4, 2)],
                           color=[1.0, 0.55, 0.25], radius=5.0, intensity=2.6))
        lights.append(dict(pos=[round(ix0 + 0.8, 2), round(fy + 1.75, 2),
                                round(cz, 2)],
                           color=[1.0, 0.75, 0.45], radius=3.6, intensity=1.4))
    if "barn" in pts:
        bx0, bz0, fy = pts["barn"]
        lights.append(dict(pos=[round(bx0 + 1.3, 2), round(fy + 1.0, 2),
                                round(bz0 + 1.6, 2)],
                           color=[1.0, 0.68, 0.38], radius=3.6, intensity=1.3))
    if "windmill" in pts:
        cx, cz, fy = pts["windmill"]
        lights.append(dict(pos=[round(cx, 2), round(fy + 1.2, 2), round(cz, 2)],
                           color=[1.0, 0.7, 0.4], radius=3.2, intensity=1.2))
    def near(a, b):
        return (abs(a[0] - b[0]) < 0.01 and abs(a[1] - b[1]) < 0.01
                and abs(a[2] - b[2]) < 0.01)

    old = man.get("lights", [])
    man["lights"] = lights
    with open(os.path.join(SCENERY, "world.json"), "w", encoding="utf-8") as f:
        json.dump(man, f, indent=1)
    gone = [l for l in old
            if not any(near(l["pos"], n["pos"]) for n in lights)]
    print(f"authored lights: {len(lights)} authoritative, {len(gone)} removed")


if __name__ == "__main__":
    main()
