#!/usr/bin/env python3
# strip_stamp.py - remove a corner watermark/logo from a candidate material
# texture so it can be tiled in the world.
#
# AI image generators (Microsoft Copilot in particular) stamp a small logo in
# a corner of every output. Sampled triplanar at ~1-3 m/tile the stamp would
# repeat across the entire terrain, so it must go before the texture is
# usable.
#
# Detection: the stamp is a compact, solid, *large-scale* bright region, so it
# survives a strong blur while the texture's own high-frequency detail does
# not. The mask is the blurred-luma > threshold set inside the top-right
# quadrant, unioned with a conservative fixed fallback region (the stamp
# position is generator-stable).
#
# Repair: clone from the same image rolled by half in both axes - the source
# content is statistically identical texture, and the mask is Gaussian
# feathered so the clone boundary has no hard seam. No external deps beyond
# numpy + PIL.
#
# Usage:
#   python3 tools/strip_stamp.py IN.png OUT.png
#   python3 tools/strip_stamp.py --report IN.png OUT.png
#   python3 tools/strip_stamp.py --feather 24 --threshold 0.45 IN.png OUT.png
import argparse
import os
import sys

import numpy as np
from PIL import Image, ImageFilter

LUMA = np.array([0.2126, 0.7152, 0.0722])


def detect_stamp(l, blur=20, thresh=None, union_fallback=True):
    """Return a bool mask of the corner stamp in the top-right quadrant.

    The stamp is found on a high-pass of the *blurred* luminance: a strong
    blur (20 px) removes the texture's own detail, and subtracting a much
    larger blur (80 px) removes any baked lighting gradient, so what is left
    is the stamp's large solid blob. The conservative generator-box region is
    unioned in by default - these stamps are generator-stable and a partial
    removal is worse than a slightly generous one.
    """
    h, w = l.shape
    bg = np.asarray(
        Image.fromarray((l * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(blur)),
        dtype=np.float64) / 255.0
    bg2 = np.asarray(
        Image.fromarray((l * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(80)),
        dtype=np.float64) / 255.0
    d = bg - bg2
    t = thresh if thresh is not None else 0.12
    m = d > t
    # Search box: the stamp is generator-stable in the top-right corner. It is
    # kept slightly larger than the fallback box but tight enough that a bright
    # patch of texture can never drag the mask deep into the image.
    m[int(h * 0.12):, :] = False
    m[:, :int(w * 0.78)] = False
    # conservative fallback: the generator's stamp box
    fb = np.zeros_like(m)
    fb[:int(h * 0.075), int(w * 0.82):] = True
    detected = bool(m.sum())
    if union_fallback or not detected:
        m = m | fb
    if not detected and not union_fallback:
        return fb, (0, 0, 0, 0), t, True
    ys, xs = np.where(m)
    y0, y1, x0, x1 = ys.min(), ys.max(), xs.min(), xs.max()
    return m, (int(y0), int(y1), int(x0), int(x1)), t, not detected


def strip(path_in, path_out, feather=14, thresh=None, pad=8, report=False,
          union_fallback=True):
    im = Image.open(path_in).convert("RGB")
    a = np.asarray(im, dtype=np.float64) / 255.0
    h, w, _ = a.shape
    l = a @ LUMA

    m, _bbox, t, fallback = detect_stamp(l, thresh=thresh, union_fallback=union_fallback)
    ys, xs = np.where(m)
    y0, y1, x0, x1 = int(ys.min()), int(ys.max()), int(xs.min()), int(xs.max())
    y0 = max(0, y0 - pad)
    y1 = min(h - 1, y1 + pad)
    x0 = max(0, x0 - pad)
    x1 = min(w - 1, x1 + pad)
    mask = np.zeros((h, w), dtype=np.float64)
    mask[y0:y1 + 1, x0:x1 + 1] = 1.0
    mask = np.asarray(
        Image.fromarray((mask * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(feather)),
        dtype=np.float64) / 255.0

    src = np.roll(a, (h // 2, w // 2), axis=(0, 1))
    out = a * (1.0 - mask[..., None]) + src * mask[..., None]
    out8 = (out.clip(0, 1) * 255).round().astype(np.uint8)
    Image.fromarray(out8).save(path_out)

    before = a[y0:y1 + 1, x0:x1 + 1] @ LUMA
    after = out[y0:y1 + 1, x0:x1 + 1] @ LUMA
    if report:
        print(f"{os.path.basename(path_in)} -> {os.path.basename(path_out)}")
        print(f"  stamp bbox y {y0}..{y1}  x {x0}..{x1}  ({y1-y0+1}x{x1-x0+1} px, "
              f"{(y1-y0+1)*(x1-x0+1)/(h*w)*100:.1f}% of image)"
              f"{'  [fallback region]' if fallback else f'  (thresh {t:.2f})'}")
        print(f"  region luma max {before.max():.3f} -> {after.max():.3f}   "
              f"mean {before.mean():.3f} -> {after.mean():.3f}")
    return (y0, y1, x0, x1), fallback, float(before.max()), float(after.max())


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--feather", type=int, default=14, help="mask feather radius (px)")
    ap.add_argument("--pad", type=int, default=8, help="bbox padding (px)")
    ap.add_argument("--threshold", type=float, default=None, help="blurred-luma threshold")
    ap.add_argument("--no-fallback", action="store_true",
                    help="do not union the conservative generator-box region")
    ap.add_argument("--report", action="store_true")
    args = ap.parse_args()
    strip(args.src, args.dst, args.feather, args.threshold, args.pad, args.report,
          not args.no_fallback)
    return 0


if __name__ == "__main__":
    sys.exit(main())
