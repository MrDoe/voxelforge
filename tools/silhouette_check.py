#!/usr/bin/env python3
"""Compare rendered silhouettes between the splat backend and the SVO reference.

Both backends read the same world, so any difference in a *shape's* rendered
outline is a rasterization artifact, not geometry. A raw per-pixel diff is
useless for this (the two backends shade differently everywhere: AO, fog,
texture filtering), so this tool segments the foreground by luminance - the
rig is dark against a bright sky in both backends - and compares the
per-band extents instead.

Usage:
  tools/silhouette_check.py ref.ppm test.ppm            # band report
  tools/silhouette_check.py ref.ppm test.ppm --mask out.png
  tools/silhouette_check.py ref.ppm test.ppm --tl 200 400 --br 700 1100

The band report is how the "splats on edges are too large" regression is
measured: the same voxel rail should cover the same pixel height in both
backends. A constant additive inflation (e.g. +0.18 m per dimension) means
the surfel disk overhangs the voxel boundary; a multiplicative one means a
scale bug.
"""
import argparse

import numpy as np
from PIL import Image


def load(path):
    return np.asarray(Image.open(path).convert("RGB")).astype(np.float32)


def luminance(im):
    return (0.299 * im[:, :, 0] + 0.587 * im[:, :, 1] + 0.114 * im[:, :, 2])


def band_report(mask, min_run=3, x_gap=25):
    """Split a boolean mask into row-contiguous bands, splitting on x-gaps."""
    H, W = mask.shape
    rows = mask.sum(1)
    out = []
    i = 0
    while i < H:
        if rows[i] <= min_run:
            i += 1
            continue
        j = i
        while j < H and rows[j] > min_run:
            j += 1
        idx = np.nonzero(mask[i:j].any(0))[0]
        groups, s, p = [], idx[0], idx[0]
        for k in idx[1:]:
            if k > p + x_gap:
                groups.append((s, p))
                s = k
            p = k
        groups.append((s, p))
        for (x0, x1) in groups:
            sub = mask[i:j, x0:x1 + 1]
            ys = np.nonzero(sub.any(1))[0]
            out.append(dict(y0=int(i + ys[0]), y1=int(i + ys[-1]),
                            x0=int(x0), x1=int(x1),
                            h=int(ys[-1] - ys[0] + 1), w=int(x1 - x0 + 1),
                            area=int(sub.sum())))
        i = j
    return out


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("reference", help="ground-truth PPM (usually --mode svo)")
    ap.add_argument("test", help="PPM to evaluate (usually --mode splat)")
    ap.add_argument("--thresh", type=float, default=135.0,
                    help="luminance below this = foreground (both backends)")
    ap.add_argument("--tl", nargs=2, type=int, metavar=("Y", "X"),
                    help="crop top-left pixel before analysing")
    ap.add_argument("--br", nargs=2, type=int, metavar=("Y", "X"),
                    help="crop bottom-right pixel before analysing")
    ap.add_argument("--mask", metavar="PNG",
                    help="save a side-by-side mask image (left=test, right=ref)")
    args = ap.parse_args()

    ref = load(args.reference)
    tst = load(args.test)
    if args.tl or args.br:
        y0, x0 = (args.tl or (0, 0))
        y1, x1 = (args.br or (ref.shape[0], ref.shape[1]))
        ref, tst = ref[y0:y1, x0:x1], tst[y0:y1, x0:x1]

    m_ref = luminance(ref) < args.thresh
    m_tst = luminance(tst) < args.thresh
    br, bt = band_report(m_ref), band_report(m_tst)

    print(f"reference {args.reference}: {m_ref.sum()} fg px, {len(br)} band(s)")
    print(f"test      {args.test}: {m_tst.sum()} fg px, {len(bt)} band(s)")
    print("\nreference bands:")
    for b in br:
        print("  y[%4d..%4d] x[%4d..%4d] h=%3d w=%3d area=%d"
              % (b["y0"], b["y1"], b["x0"], b["x1"], b["h"], b["w"], b["area"]))
    print("\ntest bands:")
    for b in bt:
        print("  y[%4d..%4d] x[%4d..%4d] h=%3d w=%3d area=%d"
              % (b["y0"], b["y1"], b["x0"], b["x1"], b["h"], b["w"], b["area"]))

    # match bands by vertical overlap (they are the same shapes, same camera)
    print("\npaired bands (test vs reference):")
    for b in bt:
        best = None
        for r in br:
            ov = min(b["y1"], r["y1"]) - max(b["y0"], r["y0"])
            if ov < 0:
                continue
            score = ov
            if best is None or score > best[0]:
                best = (score, r)
        if best is None:
            print("  y[%4d..%4d] h=%3d w=%3d  -> NO REFERENCE BAND"
                  % (b["y0"], b["y1"], b["h"], b["w"]))
            continue
        r = best[1]
        dh = b["h"] - r["h"]
        dw = b["w"] - r["w"]
        pct = lambda d, base: f"{d:+d}px ({100.0 * d / max(base, 1):+.0f}%)"
        print("  y[%4d..%4d] h=%3d w=%3d  vs ref h=%3d w=%3d  dh=%s dw=%s"
              % (b["y0"], b["y1"], b["h"], b["w"], r["h"], r["w"],
                 pct(dh, r["h"]), pct(dw, r["w"])))

    inside = m_ref
    if inside.sum():
        missing = int((~m_tst & inside).sum())
        print("\nreference fg: %d px; test missing inside it: %d (%.1f%%)"
              % (inside.sum(), missing, 100.0 * missing / inside.sum()))
    over = int((m_tst & ~m_ref).sum())
    print("test outside reference silhouette: %d px (%.0f%% of reference)"
          % (over, 100.0 * over / max(m_ref.sum(), 1)))

    if args.mask:
        side = np.concatenate([(m_tst * 255).astype(np.uint8),
                               (m_ref * 255).astype(np.uint8)], axis=1)
        Image.fromarray(side).save(args.mask)
        print(f"\nwrote {args.mask} (left = test, right = reference)")


if __name__ == "__main__":
    main()
