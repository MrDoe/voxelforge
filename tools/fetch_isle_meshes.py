#!/usr/bin/env python3
"""Stage the LighthouseIsle CC0 meshes (download once, cached).

Quaternius (CC0) publishes its free packs as public Google Drive folders
linked straight from the pack pages; gdown mirrors a folder without a
browser. This script fetches the three packs used by the scenery, copies the
five OBJ+MTL pairs into scenery/LighthouseIsle/meshes/ (renamed to isle_*,
mtllib lines rewritten — that is where the import in isle_meshes.sh reads
them from), and writes a LICENSE note beside them.

Run:  python3 tools/fetch_isle_meshes.py   (needs network + gdown venv;
      re-run with --force to refresh the cache)
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# vf_mesh2vox resolves "models/<name>.obj" against the SHARED library
# (assets/models), so that is where the pairs are staged (tracked there, like
# every other import source).
OUT = os.path.join(ROOT, "assets", "models")
LIC = os.path.join(ROOT, "scenery", "LighthouseIsle", "meshes")
CACHE = os.path.expanduser("~/.cache/vf_isle_meshes")

# pack -> (Drive folder id, {staged name: OBJ name inside the zip/folder})
PACKS = {
    "ships": ("1Qf31QTnGfxRzYxx8dHmlVGDT4KmIa0Vy",
              {"isle_sailing": "Sail ship.obj", "isle_rowboat": "Lifeboat.obj"}),
    "buildings": ("1uWCsV9QfnAu8u_QbwkYwOfuU3McME7CA",
                  {"isle_house": "House1.obj"}),
    "farmbuildings": ("1gdZ39vcLML_ULU5sHirkKQ-gEkk458Ak",
                      {"isle_barn": "BigBarn.obj", "isle_windmill": "Windmill.obj"}),
}
PROVENANCE = """These OBJ/MTL files are Quaternius (CC0, public domain):
  Ships, Buildings and Farm Buildings packs,
  https://quaternius.com (Drive folders linked from the pack pages).
Renamed to isle_* with rewritten mtllib references; geometry otherwise
unmodified. Imported via tools/.. > scenery/LighthouseIsle/isle_meshes.sh.
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


def fetch_folder(py, folder_id, dest):
    if os.path.isdir(dest) and os.listdir(dest):
        return
    print("downloading Drive folder", folder_id, "->", dest)
    subprocess.run([py, "-m", "gdown", "--folder",
                    "https://drive.google.com/drive/folders/" + folder_id,
                    "-O", dest], check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    venv = os.path.join(os.path.expanduser("~"), ".cache", "vf_isle_venv")
    py = ensure_gdown(venv)
    if args.force and os.path.isdir(CACHE):
        shutil.rmtree(CACHE)
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(LIC, exist_ok=True)
    for pack, (fid, wanted) in PACKS.items():
        tree = os.path.join(CACHE, pack)
        fetch_folder(py, fid, tree)
        objdir = None
        for dirpath, _dirs, files in os.walk(tree):
            if any(f.endswith(".obj") for f in files) and "OBJ" in dirpath.upper():
                objdir = dirpath
                break
        if objdir is None:  # any dir with the wanted file
            for dirpath, _dirs, files in os.walk(tree):
                if any(f.endswith(".obj") for f in files):
                    objdir = dirpath
                    break
        assert objdir, "no OBJ dir found in " + tree
        for staged, objname in wanted.items():
            src = None
            for dirpath, _dirs, files in os.walk(tree):
                if objname in files:
                    src = os.path.join(dirpath, objname)
                    break
            assert src, objname + " missing in cache"
            base = os.path.splitext(objname)[0]
            mtl_src = os.path.join(os.path.dirname(src), base + ".mtl")
            shutil.copyfile(src, os.path.join(OUT, staged + ".obj"))
            with open(os.path.join(OUT, staged + ".obj"), "r") as f:
                body = f.read()
            body = re.sub(r"^mtllib .*$", "mtllib " + staged + ".mtl",
                          body, flags=re.M)
            with open(os.path.join(OUT, staged + ".obj"), "w") as f:
                f.write(body)
            if os.path.exists(mtl_src):
                shutil.copyfile(mtl_src, os.path.join(OUT, staged + ".mtl"))
            print("staged", staged)
    with open(os.path.join(LIC, "LICENSE.txt"), "w") as f:
        f.write(PROVENANCE)
    print("meshes staged into", OUT)
    print("next: run scenery/LighthouseIsle/isle_meshes.sh to import layers")


if __name__ == "__main__":
    main()
