#!/usr/bin/env python3
# fetch_textures.py - download CC0 photo textures (ambientCG) and bake them
# into the material texture slots.
#
# The renderer samples a material's PNG as its albedo, so a raw photo would
# change each material's colour identity. Every download is therefore
# mean-matched to the material's palette target (the colour the palette +
# detail accents produced): the photo keeps its structure and hue variation,
# the material keeps its identity. That is the "same look, more detail"
# contract - see TARGETS in gen_textures.py, which this script imports.
#
# Output: <assets>/textures/<name>.png (512^2, PNG, seamless CC0 source).
# Sources: ambientCG (CC0 1.0). Downloads are cached under ~/.cache/vf_textures.
#
# Usage: python3 tools/fetch_textures.py [--out DIR] [--size N] [--only NAME]
#        ninja -C build textures        (runs the offline generator instead)

import argparse
import io
import json
import os
import sys
import urllib.parse
import urllib.request
import zipfile

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_textures import TARGETS  # noqa: E402  (single source of truth)

CACHE = os.path.join(os.path.expanduser("~"), ".cache", "vf_textures")
API = "https://ambientcg.com/api/v2/full_json"

# material slot -> ambientCG asset id (all CC0). Chosen by hand: seamless,
# category-appropriate, and popular enough to be stable.
SOURCES = {
    "grass_dark": "Grass001",
    "grass_light": "Grass005",
    "soil": "Ground037",
    "sand": "Ground054",
    "rock": "Rock058",
    "light_rock": "Rock051",
    "wood": "Planks037A",
    "wood_bark": "Bark014",
    "shingles": "RoofingTiles012B",
    "foliage": "Foliage006",
    "snow": "Snow010A",
}


def download_zip(asset_id, resolution="1K"):
    os.makedirs(CACHE, exist_ok=True)
    path = os.path.join(CACHE, "%s_%s-JPG.zip" % (asset_id, resolution))
    if os.path.exists(path) and os.path.getsize(path) > 10000:
        return path
    url = "https://ambientcg.com/get?file=%s_%s-JPG.zip" % (asset_id, resolution)
    print("  downloading %s ..." % url)
    req = urllib.request.Request(url, headers={"User-Agent": "voxelforge-fetch/1.0"})
    with urllib.request.urlopen(req, timeout=120) as r:
        data = r.read()
    with open(path, "wb") as f:
        f.write(data)
    return path


def extract_color(zip_path):
    with zipfile.ZipFile(zip_path) as z:
        names = z.namelist()
        cand = [n for n in names
                if "_color" in n.lower() and n.lower().endswith((".jpg", ".png"))]
        if not cand:
            raise RuntimeError("no Color map in %s (has %s)" % (zip_path, names[:6]))
        with z.open(cand[0]) as f:
            return Image.open(io.BytesIO(f.read())).convert("RGB")


def mean_match(img, target, clamp=(0.3, 4.0)):
    """Scale each channel so the image mean is the material target. Relative
    channel variation (the photo's own hue detail) is preserved."""
    a = np.asarray(img).astype(np.float64)
    src = a.reshape(-1, 3).mean(axis=0)
    tgt = np.asarray(target, dtype=np.float64)
    gain = tgt / np.maximum(src, 1e-3)
    gain = np.clip(gain, clamp[0], clamp[1])
    out = np.clip(a * gain[None, None, :], 0.0, 255.0)
    return Image.fromarray(np.round(out).astype(np.uint8), "RGB"), gain


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "assets"))
    ap.add_argument("--size", type=int, default=512)
    ap.add_argument("--only", default=None, help="one slot name")
    args = ap.parse_args()
    out_dir = os.path.join(args.out, "textures")
    os.makedirs(out_dir, exist_ok=True)
    names = [args.only] if args.only else sorted(SOURCES)
    for name in names:
        asset = SOURCES[name]
        target = TARGETS.get(name)
        if target is None:
            # wood_bark / other alternates: let the photo keep its own colour
            target = None
        try:
            z = download_zip(asset)
            img = extract_color(z)
        except Exception as e:
            print("FAIL %-12s (%s): %s" % (name, asset, e))
            continue
        img = img.resize((args.size, args.size), Image.LANCZOS)
        gain = (1.0, 1.0, 1.0)
        if target is not None:
            img, gain = mean_match(img, target)
        path = os.path.join(out_dir, name + ".png")
        img.save(path)
        arr = np.asarray(img).astype(np.float64)
        print("%-12s <- %-16s %dx%d  mean %s  gain %s"
              % (name, asset, args.size, args.size,
                 tuple(int(v) for v in arr.reshape(-1, 3).mean(axis=0).round()),
                 tuple(round(float(g), 2) for g in gain)))


if __name__ == "__main__":
    main()
