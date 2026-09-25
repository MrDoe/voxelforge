#!/usr/bin/env python3
# check_texture.py - conformance gate for material textures dropped into
# assets/textures/ (bound by the world.json "textures" table).
#
# The renderer samples a material PNG triplanar in world space at a scale of
# ~1-3 m/tile (world.json "scale"), repeated infinitely across the terrain.
# That places four hard requirements on any source image - AI-generated or
# photographed - which this tool checks BEFORE the file reaches the manifest:
#
#   1. format     PNG/JPG, >= 512x512, no alpha channel required.
#   2. tileable   The border must wrap: mean |top-bottom| and |left-right|
#                 edge difference <= ~1.35x the interior adjacent-pixel
#                 difference. A worse ratio repeats a visible grid every
#                 0.9-2.8 m across the whole world.
#   3. albedo     A least-squares luminance plane fit must be weak (gradient
#                 <= ~0.35x the image contrast). A strong gradient is baked
#                 directional sunlight: the sun is fixed (34 deg / 238 deg)
#                 and the camera moves, so baked light contradicts the real
#                 shading and breaks PBR energy conservation.
#   4. no stamp   AI generators (and stock sites) stamp corner watermarks /
#                 logos. A corner holding a disproportionate share of the
#                 image's brightest pixels is rejected.
#   5. identity   (with --material NAME) after mean-matching to the material
#                 palette target (tools/gen_textures.py TARGETS) the mean
#                 must land within tolerance - the "same look, more detail"
#                 contract that keeps the scene's colour identity.
#
# Usage:
#   python3 tools/check_texture.py FILE...                 # report
#   python3 tools/check_texture.py --material shingles FILE
#   python3 tools/check_texture.py --json FILE...
#   python3 tools/check_texture.py --quiet FILE...         # exit code only
#
# Exit code 0 = all files pass, 1 = at least one fails.
import argparse
import json
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_textures import TARGETS  # noqa: E402  (single source of truth)

MIN_SIZE = 512
# tileability: wrap edge difference / interior adjacent difference
SEAM_RATIO_MAX = 1.35
# baked light: luminance plane gradient / luminance contrast
LIGHT_RATIO_MAX = 0.35
# watermark: share of the image's brightest 0.5% pixels allowed in one corner
CORNER_BRIGHT_SHARE_MAX = 0.25
# identity: Euclidean RGB distance to the mean-matched target (0-255 units)
IDENTITY_TOL = 26.0

LUMA = np.array([0.2126, 0.7152, 0.0722])


def load(path, n=512):
    im = Image.open(path)
    has_alpha = im.mode in ("RGBA", "LA") or "transparency" in im.info
    im = im.convert("RGB")
    w, h = im.size
    return np.asarray(im.resize((n, n), Image.Resampling.LANCZOS), dtype=np.float64) / 255.0, w, h, has_alpha


def luminance(a):
    return a @ LUMA


def check_tileable(a):
    """Wrap edge difference vs interior adjacent difference."""
    d_tb = np.abs(a[0] - a[-1]).mean()
    d_lr = np.abs(a[:, 0] - a[:, -1]).mean()
    d_int = (np.abs(a[1:] - a[:-1]).mean() + np.abs(a[:, 1:] - a[:, :-1]).mean()) / 2.0
    wrap = (d_tb + d_lr) / 2.0
    ratio = wrap / max(d_int, 1e-6)
    return ratio, wrap, d_int


def check_albedo(a):
    """Least-squares plane fit to luminance: strong slope = baked sunlight."""
    l = luminance(a)
    n = l.shape[0]
    yy, xx = np.mgrid[0:n, 0:n] / float(n)
    A = np.stack([xx.ravel(), yy.ravel(), np.ones(n * n)], 1)
    coef, *_ = np.linalg.lstsq(A, l.ravel(), rcond=None)
    grad = float(np.hypot(coef[0], coef[1]))
    contrast = float(l.std())
    return grad / max(contrast, 1e-6), grad, contrast


def check_corner_stamp(a):
    """A corner holding most of the image's brightest pixels = watermark."""
    l = luminance(a)
    n = l.shape[0]
    thr = np.percentile(l, 99.5)
    bright = l > thr
    total = bright.sum()
    if total == 0:
        return 0.0, None
    c = int(n * 0.18)
    corners = {
        "TL": bright[:c, :c], "TR": bright[:c, -c:],
        "BL": bright[-c:, :c], "BR": bright[-c:, -c:],
    }
    shares = {k: float(v.sum()) / float(total) for k, v in corners.items()}
    worst = max(shares.items(), key=lambda kv: kv[1])
    return float(worst[1]), worst[0]


def check_identity(a, material):
    """Mean-match to the palette target, then measure the residual."""
    if material is None or material not in TARGETS:
        return None
    m = a.reshape(-1, 3).mean(0) * 255.0
    tgt = np.asarray(TARGETS[material], dtype=np.float64)
    gain = tgt / np.maximum(m, 1e-6)
    gain = np.clip(gain, 0.3, 4.0)
    matched = (a * gain).clip(0, 1)
    resid = float(np.linalg.norm(matched.reshape(-1, 3).mean(0) * 255.0 - tgt))
    return resid


def evaluate(path, material=None):
    a, w, h, alpha = load(path)
    seam_ratio, wrap, interior = check_tileable(a)
    light_ratio, grad, contrast = check_albedo(a)
    stamp_share, stamp_corner = check_corner_stamp(a)
    resid = check_identity(a, material)
    mean = (a.reshape(-1, 3).mean(0) * 255).round().astype(int)
    issues = []
    if w < MIN_SIZE or h < MIN_SIZE:
        issues.append(f"size {w}x{h} < {MIN_SIZE}")
    if seam_ratio > SEAM_RATIO_MAX:
        issues.append(f"not tileable (seam {seam_ratio:.2f}x > {SEAM_RATIO_MAX})")
    if light_ratio > LIGHT_RATIO_MAX:
        issues.append(f"baked lighting (gradient {light_ratio:.2f}x > {LIGHT_RATIO_MAX})")
    if stamp_share > CORNER_BRIGHT_SHARE_MAX:
        issues.append(f"corner stamp ({stamp_corner} holds {stamp_share*100:.0f}% of bright pixels)")
    if resid is not None and resid > IDENTITY_TOL:
        issues.append(f"off-palette after mean-match (residual {resid:.0f} > {IDENTITY_TOL:.0f})")
    return {
        "file": path, "size": [w, h], "alpha": alpha, "mean": mean.tolist(),
        "seam_ratio": round(seam_ratio, 3), "light_ratio": round(light_ratio, 3),
        "stamp_share": round(stamp_share, 3), "stamp_corner": stamp_corner,
        "identity_residual": None if resid is None else round(resid, 1),
        "material": material, "pass": not issues, "issues": issues,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("--material", help="expected material name (TARGETS key)")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    results = [evaluate(f, args.material) for f in args.files]
    if args.json:
        print(json.dumps(results, indent=2))
    elif not args.quiet:
        for r in results:
            status = "PASS" if r["pass"] else "FAIL"
            print(f"[{status}] {os.path.basename(r['file'])}  "
                  f"{r['size'][0]}x{r['size'][1]}  mean {tuple(r['mean'])}")
            print(f"        seam {r['seam_ratio']:.2f}x  light {r['light_ratio']:.2f}x  "
                  f"stamp {r['stamp_share']*100:.0f}%"
                  + (f"  identity {r['identity_residual']:.0f}" if r["identity_residual"] is not None else ""))
            for i in r["issues"]:
                print(f"        - {i}")
    return 0 if all(r["pass"] for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
