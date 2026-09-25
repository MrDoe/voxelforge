#!/usr/bin/env python3
# prepare_texture.py - turn a raw candidate image (photo, AI generation,
# scan) into a conformant material texture for assets/textures/.
#
# Runs the three repairs the renderer's sampling model requires, in order:
#
#   1. strip-stamp  remove a generator watermark/logo from the corner
#                   (tools/strip_stamp.py).
#   2. flatten      subtract the fitted luminance plane. A strong gradient is
#                   baked directional sunlight; the sun is fixed (34/238 deg)
#                   and the camera moves, so baked light contradicts the real
#                   shading. Applied as a multiplicative gain so the texture's
#                   colour ratios survive.
#   3. seamless     offset by half so the tile edges become adjacent rows of
#                   the original (wrap-continuous by construction), then heal
#                   the discontinuity the offset moved into the middle by
#                   cloning a shifted copy through a feathered cross mask.
#
# Reports the conformance metrics before/after (tools/check_texture.py) so a
# pass/fail is visible without a second invocation.
#
# Usage:
#   python3 tools/prepare_texture.py IN OUT [--material NAME] [--no-seamless]
#                               [--no-flatten] [--no-strip] [--report]
import argparse
import os
import sys

import numpy as np
from PIL import Image, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_texture import check_albedo, check_corner_stamp, check_identity, check_tileable  # noqa: E402
from strip_stamp import LUMA, strip  # noqa: E402


def flatten(a):
    """Remove the fitted luminance plane with a multiplicative gain."""
    l = a @ LUMA
    h, w = l.shape
    yy, xx = np.mgrid[0:h, 0:w] / float(h)
    A = np.stack([xx.ravel(), yy.ravel(), np.ones(h * w)], 1)
    coef, *_ = np.linalg.lstsq(A, l.ravel(), rcond=None)
    plane = (coef[0] * xx + coef[1] * yy + coef[2]).clip(1e-3, None)
    gain = np.clip(float(np.mean(plane)) / plane, 0.25, 4.0)
    return (a * gain[..., None]).clip(0, 1)


def make_seamless(a, band=72, feather=22):
    """Offset by half, then heal the moved discontinuity with a feathered clone."""
    h, w, _ = a.shape
    b = np.roll(a, (h // 2, w // 2), axis=(0, 1))
    mask = np.zeros((h, w), dtype=np.float64)
    mask[max(0, h // 2 - band):h // 2 + band, :] = 1.0
    mask[:, max(0, w // 2 - band):w // 2 + band] = 1.0
    mask = np.asarray(
        Image.fromarray((mask * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(feather)),
        dtype=np.float64) / 255.0
    src = np.roll(b, (h // 4, w // 4), axis=(0, 1))
    return (b * (1.0 - mask[..., None]) + src * mask[..., None]).clip(0, 1)


def metrics(a, material=None):
    sr, _, _ = check_tileable(a)
    lr, _, _ = check_albedo(a)
    ss, corner = check_corner_stamp(a)
    idr = check_identity(a, material)
    return {"seam": sr, "light": lr, "stamp": ss, "corner": corner, "identity": idr}


def prepare(src, dst, material=None, do_strip=True, do_flatten=True, do_seamless=True,
            report=False):
    tmp = dst + ".tmp.png"
    stage = src
    if do_strip:
        strip(src, tmp)
        stage = tmp
    a = np.asarray(Image.open(stage).convert("RGB"), dtype=np.float64) / 255.0
    before = metrics(a, material)
    if do_flatten:
        a = flatten(a)
    if do_seamless:
        a = make_seamless(a)
    after = metrics(a, material)
    Image.fromarray((a * 255).round().astype(np.uint8)).save(dst)
    if do_strip and os.path.exists(tmp):
        os.remove(tmp)

    if report:
        print(f"{os.path.basename(src)} -> {os.path.basename(dst)}"
              + (f"  [material {material}]" if material else ""))
        for k in ("seam", "light", "stamp"):
            print(f"  {k:6s} {before[k]:6.2f} -> {after[k]:6.2f}")
        if after["identity"] is not None:
            print(f"  identity residual {after['identity']:.1f} "
                  f"(tol {26.0:.0f}, lower is better)")
    return before, after


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--material", default=None, help="palette material name (TARGETS key)")
    ap.add_argument("--no-strip", action="store_true")
    ap.add_argument("--no-flatten", action="store_true")
    ap.add_argument("--no-seamless", action="store_true")
    ap.add_argument("--report", action="store_true")
    args = ap.parse_args()
    prepare(args.src, args.dst, args.material, not args.no_strip,
            not args.no_flatten, not args.no_seamless, args.report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
