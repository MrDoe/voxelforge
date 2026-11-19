#!/usr/bin/env python3
"""Pack photoreal CC0 foliage atlases into RGBA card layers for the texture atlas.

Each 3DTexel atlas ships a basecolor sheet and a separate opacity (silhouette)
sheet. This tool multiplies them into a single RGBA image: RGB = basecolor,
A = opacity, resampled to the atlas layer size (512x512). The result is written
to assets/textures/cards/ and a small JSON manifest maps a friendly card key to
its atlas layer index (layers 23+, after the 23 material slots).

The renderer's cardPass() samples these with clamped UVs over crossed quads,
replacing the procedural tuftAlpha() silhouette when render bit 13 is on.

Source: /home/christoph/code/plantcards-texel/ (54 CC0 maps, SOURCE.md).
Re-run this whenever the downloaded set changes; the atlas loader reads the
cards/ folder at startup.

Usage:
    python3 tools/pack_foliage_cards.py                    # default set
    python3 tools/pack_foliage_cards.py --src DIR --out DIR
    python3 tools/pack_foliage_cards.py --size 512
"""
import argparse
import json
import os

from PIL import Image

# (card key, basecolor filename, opacity filename). Ordered — index determines
# atlas layer (base 23). Keys are referenced by the shader/GUI tooltip.
DEFAULT_CARDS = [
    ("grass",      "T_Wild_Grass_01_basecolor_Jpeg.webp",        "T_Wild_Grass_01_Opacity_Jpeg.webp"),
    ("jungle",     "T_Atlases_Jungle_Leaf_basecolor_Jpeg-scaled.webp", "T_Atlases_Jungle_Leaf_Opacity_Jpeg-scaled.webp"),
    ("oak",        "T_Oak_Leaf_01_basecolor_Jpeg.webp",           "T_Oak_Leaf_01_Opacity_Jpeg.webp"),
    ("laurel",     "T_Bay_Laurel_basecolor_Jpeg.webp",            "T_Bay_Laurel_Opacity_Jpeg.webp"),
    ("nandina",    "T_Nandina_basecolor_Jpeg.webp",               "T_Nandina_Opacity_Jpeg.webp"),
]

LAYER_BASE = 23  # materials occupy 0..22


def pack(src_dir, out_dir, size, cards):
    os.makedirs(out_dir, exist_ok=True)
    by_layer = {}
    by_file = {}
    for i, (key, bc, op) in enumerate(cards):
        bc_path = os.path.join(src_dir, bc)
        op_path = os.path.join(src_dir, op)
        if not os.path.isfile(bc_path) or not os.path.isfile(op_path):
            print("! skip %-8s missing %s / %s" % (key, bc, op))
            continue
        base = Image.open(bc_path).convert("RGB").resize((size, size), Image.LANCZOS)
        alpha = Image.open(op_path).convert("L").resize((size, size), Image.LANCZOS)
        rgba = Image.merge("RGBA", (base.getchannel("R"), base.getchannel("G"),
                                    base.getchannel("B"), alpha))
        name = "card_%02d_%s.png" % (LAYER_BASE + i, key)
        rgba.save(os.path.join(out_dir, name))
        by_layer[key] = LAYER_BASE + i
        by_file[key] = name
        print("card %-8s -> layer %d  (%s)" % (key, LAYER_BASE + i, name))
    manifest = {"layerBase": LAYER_BASE, "size": size,
                "cards": by_layer, "files": by_file}
    mpath = os.path.join(out_dir, "cards.json")
    with open(mpath, "w") as f:
        json.dump(manifest, f, indent=1, sort_keys=True)
    print("manifest -> %s (%d cards)" % (mpath, len(by_layer)))
    return manifest


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", default="/home/christoph/code/plantcards-texel/maps")
    ap.add_argument("--out", default=None,
                    help="output dir (default: <repo>/assets/textures/cards)")
    ap.add_argument("--size", type=int, default=512)
    args = ap.parse_args()
    out = args.out
    if out is None:
        repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        out = os.path.join(repo, "assets", "textures", "cards")
    pack(args.src, out, args.size, DEFAULT_CARDS)


if __name__ == "__main__":
    main()
