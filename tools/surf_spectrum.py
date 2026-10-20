#!/usr/bin/env python3
"""surf_spectrum.py - where does a render's visible structure live?

A terrain artifact can live at the photo-texture scale (a few pixels), at the
surfel-disk scale (0.14 m disks, 30-90 px in the near field) or at the
landform scale (hundreds of pixels). The radial power spectrum of a luma
patch answers that in one number per band, which is what separates "the
surface is noisy" from "the surface is faceted" from "the surface is bumpy".

    surf_spectrum.py <image> <x0> <y0> <w> <h> [label]

Prints the power per spatial-frequency band, in cycles/pixel and in the
equivalent feature size in pixels. Pair it with the splat debug views
(VF_SPLAT_DEBUG=1 flat coverage, 2 normals, 5 albedo) to attribute the
structure: if the flat view is clean and the shaded view is not, the
artifact is in the shading, not in the geometry.

Usage:
    python3 tools/surf_spectrum.py build/shot.png 600 640 670 140 hero-sand
"""
import sys
import numpy as np
from PIL import Image

BANDS = [(0.002, 0.005), (0.005, 0.010), (0.010, 0.020),
         (0.020, 0.040), (0.040, 0.080), (0.080, 0.160)]


def luma(a):
    return 0.299 * a[:, :, 0] + 0.587 * a[:, :, 1] + 0.114 * a[:, :, 2]


def spectrum(path, x0, y0, w, h):
    a = np.asarray(Image.open(path).convert("RGB")).astype(float)
    p = luma(a[y0:y0 + h, x0:x0 + w])
    p = p - p.mean()
    win = np.outer(np.hanning(p.shape[0]), np.hanning(p.shape[1]))
    P = np.abs(np.fft.fftshift(np.fft.fft2(p * win))) ** 2
    H, W = P.shape
    yy, xx = np.mgrid[0:H, 0:W]
    r = np.sqrt((yy - H // 2) ** 2 + (xx - W // 2) ** 2)
    out = []
    for lo, hi in BANDS:
        m = (r >= lo * min(H, W)) & (r < hi * min(H, W))
        out.append(100.0 * P[m].sum() / P.sum())
    return out


def main():
    if len(sys.argv) < 6:
        print(__doc__)
        return 1
    path = sys.argv[1]
    x0, y0, w, h = (int(v) for v in sys.argv[2:6])
    label = sys.argv[6] if len(sys.argv) > 6 else path
    print(f"{label}: patch {w}x{h}")
    for (lo, hi), pct in zip(BANDS, spectrum(path, x0, y0, w, h)):
        print("  %6.3f-%6.3f c/px  (feature %5.1f-%5.1f px)  power %6.2f%%"
              % (lo, hi, 1.0 / hi, 1.0 / lo, pct))
    return 0


if __name__ == "__main__":
    sys.exit(main())
