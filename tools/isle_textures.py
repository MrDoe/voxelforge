#!/usr/bin/env python3
"""Stage the LighthouseIsle texture bindings (CC0 ambientCG photos).

A scenery may bind its own photos per material through its own world.json
"textures" table, and `scenery::resolveShared()` tries the SCENERY dir first,
so the isle can wear different sand than the hamlet without touching the
shared library. This script:

  1. downloads two CC0 ambientCG assets (cached under ~/.cache/vf_textures,
     same cache tools/fetch_textures.py uses): a beach sand and a rocky
     shore/cliff;
  2. mean-matches the sand to the palette sand target (the shared library's
     identity contract — photo structure, palette identity) and leaves the
     coastal rock its own colour (it is a NEW look for mats 4/5 only inside
     this manifest);
  3. resizes to the 512^2 atlas layer size and writes
     scenery/LighthouseIsle/textures/isle_sand.png (+ isle_rock_coastal.png);
  4. rewrites the scenery world.json "textures" table: the shared-library
     bindings the isle keeps (grass/bark/moss/wood/plaster/thatch/gravel/
     rock) + the two local overrides (mat 3 beach sand, mat 20 plaster stays
     on the shared photo, mats 4/5 on the coastal rock).

The app rewrites only the manifest keys it owns, and "textures" is written
back verbatim on layer saves (see the Textures panel note in AGENTS.md), so
hand-staging it once here is stable.
"""
import io
import json
import os
import sys
import urllib.request
import zipfile

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCENERY = os.path.join(ROOT, "scenery", "LighthouseIsle")
CACHE = os.path.expanduser("~/.cache/vf_textures")
sys.path.insert(0, os.path.join(ROOT, "tools"))
from gen_textures import TARGETS  # noqa: E402  palette identity targets

# ambientCG (CC0): Ground054 is the shared library's own beach-sand source —
# here mean-matched DRYER/brighter for the wide isle beach; Rock063 reads as
# a wave-worn layered coastal cliff (a NEW look for mats 4/5 inside this
# manifest only, the hamlet keeps its Rock058 identity).
FETCH = {
    "isle_sand.png": ("Ground054", (228, 214, 178)),     # mean-matched to dry sand
    "isle_rock_coastal.png": ("Rock063", None),          # photo keeps identity
}

# the bindings the isle keeps from the shared library (mat -> file, scale)
# FINAL table per the 2026-11-19 promotion war — every entry passes
# tools/check_texture.py and is mean-matched to slot identity. New names
# (grass_meadow_prepared, soil_ground037, foliage_foliage006,
# thatch_straw_prepared) are landed by tools/isle_tex_finalize.py.
SHARED = [
    {"file": "textures/grass_meadow_prepared.png", "mat": 0, "scale": 2.6},
    {"file": "textures/grass_light_high_res.png", "mat": 1, "scale": 2.6},
    {"file": "textures/soil_ground037.png", "mat": 2, "scale": 1.8},
    {"file": "textures/proc_sand.png", "mat": 3, "scale": 1.5},  # replaced below
    {"file": "textures/rock.jpeg", "mat": 4, "scale": 1.2},
    {"file": "textures/rock_grey.png", "mat": 5, "scale": 1.2},
    {"file": "textures/wood_highres.png", "mat": 6, "scale": 1.0},
    {"file": "textures/wooden_planks_dark.jpeg", "mat": 7, "scale": 0.9},
    {"file": "textures/foliage_foliage006.png", "mat": 8, "scale": 0.5},
    {"file": "textures/gravel.png", "mat": 15, "scale": 0.9},
    {"file": "textures/bark_brown.png", "mat": 17, "scale": 0.4},
    {"file": "textures/proc_moss.png", "mat": 18, "scale": 0.4},
    {"file": "textures/thatch_straw_prepared.png", "mat": 19, "scale": 0.6},
    {"file": "textures/stone_plaster.png", "mat": 20, "scale": 1.0},
]


def download(asset_id):
    os.makedirs(CACHE, exist_ok=True)
    path = os.path.join(CACHE, asset_id + "_2K-JPG.zip")
    if os.path.exists(path) and os.path.getsize(path) > 10000:
        return path
    url = "https://ambientcg.com/get?file=%s_2K-JPG.zip" % asset_id
    print("  downloading", url)
    req = urllib.request.Request(url, headers={"User-Agent": "voxelforge-isle/1.0"})
    with urllib.request.urlopen(req, timeout=180) as r:
        data = r.read()
    with open(path, "wb") as f:
        f.write(data)
    return path


def color_from_zip(zp):
    with zipfile.ZipFile(zp) as z:
        cand = [n for n in z.namelist()
                if "_color" in n.lower() and n.lower().endswith((".jpg", ".png"))]
        if not cand:
            raise RuntimeError("no color map in " + zp)
        with z.open(cand[0]) as f:
            return Image.open(io.BytesIO(f.read())).convert("RGB")


def main():
    out = os.path.join(SCENERY, "textures")
    os.makedirs(out, exist_ok=True)
    for name, (asset, target) in FETCH.items():
        dest = os.path.join(out, name)
        if os.path.exists(dest):
            print("exists", dest)
            continue
        img = color_from_zip(download(asset)).resize((512, 512), Image.LANCZOS)
        a = np.asarray(img).astype(np.float64)
        if target:
            src = a.reshape(-1, 3).mean(axis=0)
            gain = np.clip(np.asarray(target) / np.maximum(src, 1e-3), 0.3, 4.0)
            a = np.clip(a * gain[None, None, :], 0, 255)
            print(name, "mean-matched gain", np.round(gain, 2))
        Image.fromarray(np.round(a).astype(np.uint8)).save(dest)
        print("wrote", dest)

    # manifest: replace mat 3 with the local sand, keep the rest of SHARED
    entries = []
    for e in SHARED:
        if e["mat"] == 3:
            entries.append({"file": "textures/isle_sand.png", "mat": 3,
                            "scale": 0.9})   # ~10 cm grain reads at eye height
        else:
            entries.append(dict(e))
    mf = os.path.join(SCENERY, "world.json")
    with open(mf) as f:
        world = json.load(f)
    world["textures"] = entries
    with open(mf, "w") as f:
        json.dump(world, f, indent=1)
    print("manifest textures table rewritten:", len(entries), "entries")


if __name__ == "__main__":
    main()
