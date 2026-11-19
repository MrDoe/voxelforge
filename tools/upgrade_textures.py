#!/usr/bin/env python3
"""upgrade_textures.py — promote high-detail CC0 photos onto BOUND material
texture filenames, enforcing the full repo promotion contract.

Contract (corrected 2026-11-19 the hard way — mean-matching is IDENTITY,
NOT conformance; three ambientCG picks failed the gate after promotion):

    download 2K colour map -> LANCZOS 512 -> tools/prepare_texture.py
    (strip watermark, flatten lighting, seamless-ize) -> mean_match to the
    slot's palette identity -> tools/check_texture.py MUST PASS -> only then
    write the bound filename. A bound file that already passes the gate is
    an A/B tie: promote only on an explicit --force (human call).

Writes are announced with timestamps (shared-tree freeze contract): the
atlas hot-reloads on a 0.5 s mtime poll, so a write during anyone's gate
window is a live render mutation.

Sources: ambientCG (CC0 1.0). Cache: ~/.cache/vf_textures (shared).
Existence probe for new ids: api/v2/full_json?id=X&limit=1, key foundAssets.

Usage: python3 tools/upgrade_textures.py [--only NAME ...] [--force]
"""
import argparse
import json
import os
import subprocess
import sys
import time

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fetch_textures import CACHE, download_zip, extract_color, mean_match  # noqa: E402
from gen_textures import TARGETS  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEXDIR = os.path.join(ROOT, "assets", "textures")
STAGE = "/tmp/opencode/isle_tex"

# bound file -> (ambientCG id, TARGETS key or explicit mean)
SWAPS = {
    "bark_brown.png": ("Bark015", "bark"),
    "stone_plaster.png": ("Plaster003", "plaster"),   # Plaster005: gate-fail
    "gravel.png": ("Gravel027", (138, 132, 124)),     # slot's own mean today
    "proc_moss.png": ("Ground019", "moss"),
    "rock.jpeg": ("Rock058", "rock"),                 # Rock064: seam-fail
    "rock_grey.png": ("Rock063", "light_rock"),
    "wood_highres.png": ("Wood030", "wood"),
    "wooden_planks_dark.jpeg": ("Planks037A", "shingles"),  # tiles: seam-fail
}


def build(asset, target, mat):
    os.makedirs(STAGE, exist_ok=True)
    raw = extract_color(download_zip(asset, "2K")).resize((512, 512), Image.LANCZOS)
    pre = os.path.join(STAGE, f"u_{asset}.png")
    raw.save(pre)
    dst = os.path.join(STAGE, f"uprep_{asset}.png")
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "prepare_texture.py"),
                        pre, dst, "--material", mat if isinstance(mat, str) else "sand"],
                       capture_output=True, text=True)
    if r.returncode != 0:               # prepared files that refuse prep are
        dst = pre                       # gated anyway — no silent skip
    img = Image.open(dst).convert("RGB")
    img, gain = mean_match(img, target)
    return img, gain


def gate(path):
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "check_texture.py"),
                        "--json", path], capture_output=True, text=True)
    try:
        return json.loads(r.stdout)[0]
    except Exception:
        return {"pass": False, "issues": ["gate output unparseable"],
                "seam_ratio": -1, "light_ratio": -1, "mean": [0, 0, 0]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", nargs="*", default=None)
    ap.add_argument("--force", action="store_true",
                    help="promote even where the bound file already passes the gate")
    args = ap.parse_args()
    names = args.only if args.only else sorted(SWAPS)
    bak = os.path.join(CACHE, "backup-" + time.strftime("%Y%m%d"))
    os.makedirs(bak, exist_ok=True)
    for fname in names:
        asset, mat = SWAPS[fname]
        target = TARGETS[mat] if isinstance(mat, str) else mat
        path = os.path.join(TEXDIR, fname)
        cur = gate(path) if os.path.exists(path) else {"pass": False}
        if cur.get("pass") and not args.force:
            print(f"SKIP {fname}: bound file PASSES the gate — A/B tie, "
                  f"promote deliberately with --force")
            continue
        img, gain = build(asset, target, mat)
        stage = os.path.join(STAGE, f"promote_{fname}")
        img.save(stage)
        it = gate(stage)
        if not it.get("pass"):
            print(f"REJECT {fname} <- {asset}: gate FAIL {it.get('issues')}")
            continue
        if os.path.exists(path):
            shutil_copy2(path, os.path.join(bak, fname + ".pre-promotion"))
        print(f"{time.strftime('%H:%M:%S')} PROMOTING {fname} <- {asset} "
              f"(seam {it['seam_ratio']:.2f} light {it['light_ratio']:.2f} "
              f"mean {tuple(it['mean'])} gain {tuple(round(float(g), 2) for g in gain)})")
        final = Image.open(stage)
        if fname.endswith((".jpeg", ".jpg")):
            final.convert("RGB").save(path, format="JPEG", quality=92)
        else:
            final.save(path)
    print("backups: " + bak)


def shutil_copy2(a, b):
    import shutil
    shutil.copy2(a, b)


if __name__ == "__main__":
    main()
