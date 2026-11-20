#!/usr/bin/env python3
"""Stage CC0 detailed prop meshes for micro-voxel layers (download once, cached).

Quaternius (CC0) publishes its free packs as public Google Drive folders linked
from the pack pages; gdown mirrors a folder without a browser. This script
fetches the Survival pack, copies a curated set of small props into
assets/models/ (renamed micro_*), rewrites their mtllib lines, and writes a
LICENSE note. The staged meshes are the SOURCE for tools/mesh_to_micro.cpp,
which voxelizes each into a fine-lattice micro layer whose app-side meshSource
points back here.

Run:  python3 tools/fetch_micro_meshes.py          (needs network + gdown)
      python3 tools/fetch_micro_meshes.py --force  (refresh the cache)
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets", "models")
LIC = os.path.join(ROOT, "assets", "models", "micro_licenses")
CACHE = os.path.expanduser("~/.cache/vf_micro_meshes")

# Quaternius Survival pack (public Drive folder from the pack page).
FOLDER = "1NKfC95GMWWJquy6rRzwFVkVDoZ_QP7K_"

# staged name -> OBJ file inside the pack's OBJ/ dir
PROPS = {
    "micro_torch": "WoodenTorch.obj",
    "micro_compass": "Compass_Open.obj",
    "micro_radio": "Radio.obj",
    "micro_backpack": "Backpack.obj",
    "micro_pot": "Pot.obj",
    "micro_gascan": "GasCan.obj",
    "micro_log": "WoodLog.obj",
}

PROVENANCE = """These OBJ/MTL files are Quaternius (CC0 1.0 Universal, public
domain dedication): the Survival pack, https://quaternius.com
(Drive folder linked from the pack page). Staged by
tools/fetch_micro_meshes.py, renamed micro_*, mtllib lines rewritten; geometry
otherwise unmodified. They are the meshSource of raster-owned micro-voxel
layers authored with tools/mesh_to_micro.cpp. License:
https://creativecommons.org/publicdomain/zero/1.0/
"""


def ensure_gdown(venv):
    py = os.path.join(venv, "bin", "python")
    if os.path.exists(py):
        r = subprocess.run([py, "-m", "gdown", "--version"], capture_output=True)
        if r.returncode == 0:
            return py
    if not os.path.exists(venv):
        subprocess.run([sys.executable, "-m", "venv", venv], check=True)
    subprocess.run([os.path.join(venv, "bin", "pip"), "install", "--quiet", "gdown"],
                   check=True)
    return py


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    venv = os.path.join(os.path.expanduser("~"), ".cache", "vf_isle_venv")
    py = ensure_gdown(venv)
    tree = os.path.join(CACHE, "survival")
    if args.force and os.path.isdir(tree):
        shutil.rmtree(tree)
    objdir = os.path.join(tree, "OBJ")
    if not os.path.isdir(objdir):
        print("downloading Quaternius Survival pack ->", tree)
        subprocess.run([py, "-m", "gdown", "--folder",
                        "https://drive.google.com/drive/folders/" + FOLDER,
                        "-O", tree], check=True)
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(LIC, exist_ok=True)
    for staged, objname in PROPS.items():
        src = os.path.join(objdir, objname)
        if not os.path.exists(src):
            print("MISSING", src)
            continue
        base = os.path.splitext(objname)[0]
        shutil.copyfile(src, os.path.join(OUT, staged + ".obj"))
        with open(os.path.join(OUT, staged + ".obj")) as f:
            body = f.read()
        body = re.sub(r"^mtllib .*$", "mtllib " + staged + ".mtl",
                      body, flags=re.M)
        with open(os.path.join(OUT, staged + ".obj"), "w") as f:
            f.write(body)
        mtl = os.path.join(objdir, base + ".mtl")
        if os.path.exists(mtl):
            shutil.copyfile(mtl, os.path.join(OUT, staged + ".mtl"))
        print("staged", staged)
    with open(os.path.join(LIC, "LICENSE.txt"), "w") as f:
        f.write(PROVENANCE)
    print("micro prop meshes staged into", OUT)


if __name__ == "__main__":
    main()
