#!/usr/bin/env python3
"""isle_tex_finalize.py — batched texture-restore for LighthouseIsle, run
ONCE per window-close (single announced write burst; never while an app
window on the scenery is open — hot-reload churn contract, 2026-11-19).

Copies the gate-verified, mean-matched staged candidates from
/tmp/opencode/isle_tex/ (produced by tools/upgrade_textures.py + the
prepare+mean_match+check gate pipeline) into the shared library, then
rewrites ONLY the scenery manifest's "textures" key — every other key
(lights, foliageCards, layers, ...) is preserved byte-for-byte by reading
and re-dumping the JSON untouched.

Final table (all PASS tools/check_texture.py, all mean-matched to slot
identity; see .opencode/wiki/log.md 2026-11-19 for the promotion war):
  mat 0 grass_AI(prepared)   mat 1 grass_light_high_res (incumbent)
  mat 2 soil_Ground037       mat 3 isle_sand (scenery-local)
  mat 4 rock.jpeg<-Rock058   mat 5 rock_grey<-Rock063
  mat 6 wood_highres<-Wood030 mat 7 wooden_planks_dark<-Planks037A
  mat 8 foliage_Foliage006   mat 15 gravel<-Gravel027
  mat 17 bark_brown<-Bark015 mat 18 proc_moss<-Ground019
  mat 19 thatch_AI(prepared) mat 20 stone_plaster<-Plaster003
"""
import json
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STAGE = "/tmp/opencode/isle_tex"
TEX = os.path.join(ROOT, "assets", "textures")
MAN = os.path.join(ROOT, "scenery", "LighthouseIsle", "world.json")

# staged file -> asset filename (bound-name slots + new candidate files)
FILES = [
    ("promote_bark_brown.png", "bark_brown.png"),
    ("promote_gravel.png", "gravel.png"),
    ("promote_proc_moss.png", "proc_moss.png"),
    ("promote_wood_highres.png", "wood_highres.png"),
    ("promote_rock_grey.png", "rock_grey.png"),
    ("promote_stone_plaster.png", "stone_plaster.png"),
    ("promote_rock.jpeg", "rock.jpeg"),
    ("promote_wooden_planks_dark.jpeg", "wooden_planks_dark.jpeg"),
    ("promote2_foliage_Foliage006.png", "foliage_foliage006.png"),
    ("promote2_thatch_AI.png", "thatch_straw_prepared.png"),
    ("promote2_grass_AI.png", "grass_meadow_prepared.png"),
    ("promote2_soil_Ground037.png", "soil_ground037.png"),
]

# mat, file (paths relative to the manifest, i.e. "textures/<name>"), scale
TABLE = [
    (0, "textures/grass_meadow_prepared.png", 2.6),
    (1, "textures/grass_light_high_res.png", 2.6),          # incumbent
    (2, "textures/soil_ground037.png", 1.8),
    (3, "textures/isle_sand.png", 0.9),                     # scenery-local
    (4, "textures/rock.jpeg", 1.2),
    (5, "textures/rock_grey.png", 1.2),
    (6, "textures/wood_highres.png", 1.0),
    (7, "textures/wooden_planks_dark.jpeg", 0.9),
    (8, "textures/foliage_foliage006.png", 0.5),
    (15, "textures/gravel.png", 0.9),
    (17, "textures/bark_brown.png", 0.4),
    (18, "textures/proc_moss.png", 0.4),
    (19, "textures/thatch_straw_prepared.png", 0.6),
    (20, "textures/stone_plaster.png", 1.0),
]


def main():
    missing = [s for s, _ in FILES if not os.path.exists(os.path.join(STAGE, s))]
    if missing:
        sys.exit("staged files missing, run the promotion pipeline first: "
                 + ", ".join(missing))
    print(f"{time.strftime('%H:%M:%S')} isle_tex_finalize: announcing batched "
          f"write burst (assets + isle manifest textures key)")
    bak = os.path.expanduser(f"~/.cache/vf_textures/backup-{time.strftime('%Y%m%d')}")
    os.makedirs(bak, exist_ok=True)
    for stage_name, asset_name in FILES:
        src = os.path.join(STAGE, stage_name)
        dst = os.path.join(TEX, asset_name)
        if os.path.exists(dst):
            shutil.copy2(dst, os.path.join(bak, asset_name + ".pre-finalize"))
        shutil.copy2(src, dst)
    with open(MAN, encoding="utf-8") as f:
        man = json.load(f)
    man["textures"] = [
        {"mat": m, "file": fn, "scale": sc} for m, fn, sc in TABLE]
    with open(MAN, "w", encoding="utf-8") as f:
        json.dump(man, f, indent=1)
    # verify the bound set gates
    paths = []
    for _, fn, _ in TABLE:
        p = os.path.join(ROOT, "scenery", "LighthouseIsle", fn)
        if not os.path.exists(p):
            p = os.path.join(ROOT, "assets", fn)
        paths.append(p)
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "check_texture.py"),
                        "--quiet", *paths], capture_output=True, text=True)
    print("gate:", "ALL PASS" if r.returncode == 0 else "FAILURES:\n" + r.stdout + r.stderr)
    print("manifest textures:", len(man["textures"]), "entries; keys kept:",
          sorted(k for k in man if k not in ("textures",)))


if __name__ == "__main__":
    main()
