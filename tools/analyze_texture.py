#!/usr/bin/env python3
"""Programmatic texture analysis: identify material, tileability, baked light."""
import sys, os, glob
import numpy as np
from PIL import Image

def load(p, n=256):
    im = Image.open(p).convert("RGB").resize((n, n), Image.LANCZOS)
    return np.asarray(im, dtype=np.float64) / 255.0

def lum(a):
    return a @ np.array([0.2126, 0.7152, 0.0722])

def wrap_diff(a):
    """Seam score: mean |edge column/row difference| across the wrap."""
    top, bot = a[0, :, :], a[-1, :, :]
    left, right = a[:, 0, :], a[:, -1, :]
    d_tb = np.abs(top - bot).mean()
    d_lr = np.abs(left - right).mean()
    # interior adjacent-row/col difference for scale reference
    d_int = (np.abs(a[1:] - a[:-1]).mean() + np.abs(a[:, 1:] - a[:, :-1]).mean()) / 2
    return (d_tb + d_lr) / 2, d_int

def baked_light(a):
    """Least-squares plane fit to luminance -> directional gradient strength."""
    l = lum(a)
    n = l.shape[0]
    yy, xx = np.mgrid[0:n, 0:n] / n
    A = np.stack([xx.ravel(), yy.ravel(), np.ones(n * n)], 1)
    coef, *_ = np.linalg.lstsq(A, l.ravel(), rcond=None)
    gx, gy = coef[0], coef[1]
    grad = float(np.hypot(gx, gy))
    # plane amplitude vs local contrast
    contrast = float(l.std())
    return grad, contrast, grad / max(contrast, 1e-6)

def ascii_map(a, n=16):
    l = lum(a)
    s = l.shape[0] // n
    blocks = l[: s * n, : s * n].reshape(n, s, n, s).mean(axis=(1, 3))
    lo, hi = blocks.min(), blocks.max()
    ramp = " .:-=+*#%@"
    if hi - lo < 1e-6:
        return "\n".join(" " * n for _ in range(n))
    idx = ((blocks - lo) / (hi - lo) * (len(ramp) - 1)).astype(int)
    return "\n".join("".join(ramp[i] for i in row) for row in idx)

def sat(a):
    mx, mn = a.max(axis=2), a.min(axis=2)
    return float(((mx - mn) / np.maximum(mx, 1e-6)).mean())

for p in sorted(sys.argv[1:]):
    a = load(p)
    w, d = wrap_diff(a)
    g, c, ratio = baked_light(a)
    m = (a.reshape(-1, 3).mean(0) * 255).round().astype(int)
    print("=" * 66)
    print(os.path.basename(p))
    print(f"  mean RGB {tuple(m)}   sat {sat(a):.2f}   luma std {c:.3f}")
    print(f"  seam wrap-diff {w:.4f}  (interior adj {d:.4f}, ratio {w/max(d,1e-6):.2f}x)")
    print(f"  light-plane grad {g:.3f}  ratio {ratio:.2f}")
    print(ascii_map(a))
