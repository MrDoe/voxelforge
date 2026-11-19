#!/usr/bin/env python3
"""isle_tex_renew.py — user directive 2026-11-19b: delete all proc_* low-res
textures, go over EVERY isle material with fresh photo textures, and add new
textures to the shared library.

Two phases, because the live window + the (mitigated, unproven) reload race
make hot writes unsafe:

  --stage   build every candidate (download 2K -> LANCZOS 512 ->
            prepare_texture strip/flatten/seamless -> mean_match to slot
            identity -> check_texture gate). Writes only /tmp staging.
  --land    ONE announced burst: copy isle-identity files into
            scenery/LighthouseIsle/textures/, new photos into
            assets/textures/, rewrite the isle "textures" key (cards
            carried), bind the new library files where free mats allow,
            rm the proc_* files (only once NOTHING references them),
            re-gate all live bindings, print proof.

Run --land only with no app window open on a scenery whose manifest changes.
"""
import argparse
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fetch_textures import CACHE, download_zip, extract_color, mean_match  # noqa: E402
from gen_textures import TARGETS  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STAGE = "/tmp/opencode/isle_renew"
ISLE_TEX = os.path.join(ROOT, "scenery", "LighthouseIsle", "textures")
SHARED = os.path.join(ROOT, "assets", "textures")
ISLE_MAN = os.path.join(ROOT, "scenery", "LighthouseIsle", "world.json")

# (stage name, ambientCG id, TARGETS key or explicit mean, lands where)
# dest: ("isle", file) scenery-local | ("lib", file) shared library
PLAN = [
    ("isle_grass_tropical.png", "Grass001", "grass_dark", ("isle", "isle_grass_tropical.png"), 2.6, 0, "grass"),
    ("isle_grass_dune.png", "Grass005", "grass_light", ("isle", "isle_grass_dune.png"), 2.6, 1, "grass"),
    ("isle_soil.png", "Ground037", "soil", ("isle", "isle_soil.png"), 1.8, 2, None),
    # mat 3 = isle_sand already gate-passes (Ground054); kept, not rebuilt
    ("isle_rock_volcanic.png", "Rock058", "rock", ("isle", "isle_rock_volcanic.png"), 1.2, 4, None),
    ("isle_rock_cliff.png", "Rock063", "light_rock", ("isle", "isle_rock_cliff.png"), 1.2, 5, None),
    ("isle_wood_teak.png", "Wood030", "wood", ("isle", "isle_wood_teak.png"), 1.0, 6, None),
    ("isle_shingle.png", "Planks037A", "shingles", ("isle", "isle_shingle.png"), 0.9, 7, None),
    ("isle_junglefloor.png", "Foliage006", "foliage", ("isle", "isle_junglefloor.png"), 0.5, 8, "jungle"),
    ("isle_snow.png", "Snow010A", "snow", ("isle", "isle_snow.png"), 1.0, 16, None),  # NEW binding (cone cap)
    ("isle_gravel.png", "Gravel027", (138, 132, 124), ("isle", "isle_gravel.png"), 0.9, 15, None),
    ("isle_bark.png", "Bark015", "bark", ("isle", "isle_bark.png"), 0.4, 17, None),
    ("isle_moss.png", "Ground019", "moss", ("isle", "isle_moss.png"), 0.4, 18, None),
    ("isle_thatch.png", "Plaster003", "plaster", None, None, None, None),  # placeholder skipped (see note)
    ("isle_plaster.png", "Plaster003", "plaster", ("isle", "isle_plaster.png"), 1.0, 20, None),
    # shared-library additions (new textures for every world + the GUI picker)
    ("lib_sand_beach.png", "Ground054", (210, 180, 95), ("lib", "sand_beach.png"), None, None, None),
    ("lib_moss_forest.png", "Ground108", "moss", ("lib", "moss_forest.png"), None, None, None),
    ("lib_rock_shore.png", "Rock063", "light_rock", ("lib", "rock_shore.png"), None, None, None),
    ("lib_grass_meadow.png", "Grass005", "grass_light", ("lib", "grass_meadow.png"), None, None, None),
]
# thatch (mat 19): the only compliant thatch art in the repo is the prepared
# AI straw (no ambientCG thatch series exists); land it under an isle name too
THATCH_SRC = "/tmp/opencode/isle_tex/promote2_thatch_AI.png"

PROC_FILES = [f for f in sorted(os.listdir(SHARED)) if f.startswith("proc_") and f.endswith(".png")] \
    if os.path.isdir(SHARED) else []


def build(asset, tgt, mat_for_prepare):
    os.makedirs(STAGE, exist_ok=True)
    raw = extract_color(download_zip(asset, "2K")).resize((512, 512), __import__("PIL.Image", fromlist=["Image"]).LANCZOS)
    pre = os.path.join(STAGE, f"src_{asset}.png")
    raw.save(pre)
    dst = os.path.join(STAGE, f"prep_{asset}.png")
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "prepare_texture.py"),
                        pre, dst, "--material", mat_for_prepare], capture_output=True)
    img = dst if r.returncode == 0 else pre
    from PIL import Image
    im = Image.open(img).convert("RGB")
    im, _ = mean_match(im, tgt)
    out = os.path.join(STAGE, f"cand_{asset}.png")
    im.save(out)
    g = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "check_texture.py"),
                        "--json", out], capture_output=True, text=True)
    try:
        it = json.loads(g.stdout)[0]
    except Exception:
        it = {"pass": False, "issues": ["unparseable"], "seam_ratio": -1,
              "light_ratio": -1, "mean": [0, 0, 0]}
    return out, it


def stage():
    ok, bad = [], []
    for row in PLAN:
        name, asset, mat, dest, *_ = row
        if dest is None:
            continue
        tgt = TARGETS[mat] if isinstance(mat, str) else mat
        out, it = build(asset, tgt, mat if isinstance(mat, str) else "sand")
        line = "%-28s <- %-12s %s seam %.2f light %.2f mean %s" % (
            name, asset, "PASS" if it["pass"] else "FAIL", it["seam_ratio"],
            it["light_ratio"], tuple(it["mean"]))
        print(line)
        (ok if it["pass"] else bad).append((name, out, asset))
    # thatch reuse (already gate-passed at build time)
    if os.path.exists(THATCH_SRC):
        import shutil
        out = os.path.join(STAGE, "cand_thatch.png")
        shutil.copy2(THATCH_SRC, out)
        ok.append(("isle_thatch.png", out, "prepared-AI"))
        print("isle_thatch.png            <- prepared AI  PASS (reused)")
    print(f"\nstaged {len(ok)} gate-passing, {len(bad)} rejected: {[b[0] for b in bad]}")
    with open(os.path.join(STAGE, "manifest.json"), "w") as f:
        json.dump(ok, f)


def references_proc():
    """any manifest still referencing proc_* -> refuse to delete those"""
    refs = set()
    for mf in (os.path.join(ROOT, "scenery", "LakesideHamlet", "world.json"), ISLE_MAN):
        try:
            m = json.load(open(mf))
        except Exception:
            continue
        for t in m.get("textures", []):
            if "proc_" in t.get("file", ""):
                refs.add(t["file"].split("/")[-1])
    return refs


def land():
    staged = json.load(open(os.path.join(STAGE, "manifest.json")))
    by_name = {n: p for n, p, _ in staged}
    print(f"{time.strftime('%H:%M:%S')} isle_tex_renew LAND: announced burst")
    # 1) place files
    for row in PLAN:
        name, asset, mat, dest = row[:4]
        if dest is None:
            continue
        kind, fname = dest
        src = by_name.get(name)
        if not src:
            print("  skip (gate-failed earlier):", name)
            continue
        dirp = ISLE_TEX if kind == "isle" else SHARED
        os.makedirs(dirp, exist_ok=True)
        import shutil
        shutil.copy2(src, os.path.join(dirp, fname))
    if "isle_thatch.png" in by_name:
        import shutil
        shutil.copy2(by_name["isle_thatch.png"], os.path.join(ISLE_TEX, "isle_thatch.png"))
    # 2) rewrite the isle textures key (cards carried, other keys untouched)
    man = json.load(open(ISLE_MAN))
    old = {t.get("mat"): t for t in man.get("textures", [])}
    rows, seen = [], set()
    for row in PLAN:
        name, asset, mat, dest, scale, matid, card = row
        if dest is None or name not in by_name:
            continue
        kind, fname = dest
        if kind != "isle":
            continue
        rowd = {"mat": matid, "file": "textures/" + fname, "scale": scale}
        if card:
            rowd["card"] = card
        rows.append(rowd)
        seen.add(matid)
    # thatch + the already-passing isle_sand
    rows.append({"mat": 3, "file": "textures/isle_sand.png", "scale": 0.9})
    if os.path.exists(os.path.join(ISLE_TEX, "isle_thatch.png")):
        rows.append({"mat": 19, "file": "textures/isle_thatch.png", "scale": 0.6})
    rows.sort(key=lambda r: r["mat"])
    man["textures"] = rows
    with open(ISLE_MAN, "w") as f:
        json.dump(man, f, indent=1)
    print("  isle manifest textures:", len(rows), "rows; cards:",
          [(r["mat"], r.get("card")) for r in rows if r.get("card")])
    # 3) delete proc_* — only unreferenced ones
    refs = references_proc()
    deleted, kept = [], []
    for f in PROC_FILES:
        if f in refs:
            kept.append(f)
            continue
        os.remove(os.path.join(SHARED, f))
        deleted.append(f)
    print("  proc_* deleted:", len(deleted), "kept (still referenced):", kept)
    # 4) re-gate everything the isle now binds
    paths = []
    for r in rows:
        p = os.path.join(ROOT, "scenery", "LighthouseIsle", r["file"])
        if not os.path.exists(p):
            p = os.path.join(ROOT, "assets", r["file"])
        paths.append(p)
    g = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "check_texture.py"),
                        "--json", *paths], capture_output=True, text=True)
    fails = [it for it in json.loads(g.stdout) if not it["pass"]]
    print("  live isle set gate:", "ALL PASS" if not fails else
          [("F", x["file"].split("/")[-1], x["issues"]) for x in fails])


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--stage", action="store_true")
    g.add_argument("--land", action="store_true")
    a = ap.parse_args()
    stage() if a.stage else land()
