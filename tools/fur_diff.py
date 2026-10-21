#!/usr/bin/env python3
"""Compare two PPM renders numerically (the measurement, not the caption).

Usage: fur_diff.py ON.ppm OFF.ppm [--mask x0 y0 x1 y1]
Prints the changed-pixel count, the mean/max |diff| over the changed pixels,
the bounding box of the change, and the mean |diff| inside/outside a mask.

A vision caption cannot judge a 0.1% artefact and confidently mislabels the
cause, so the numbers are the instrument (see the wiki's measurement
discipline page).
"""
import sys


def read_ppm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        w, h = map(int, f.readline().split())
        assert int(f.readline()) == 255
        return w, h, f.read()


def main():
    on_path, off_path = sys.argv[1], sys.argv[2]
    w, h, a = read_ppm(on_path)
    w2, h2, b = read_ppm(off_path)
    assert (w, h) == (w2, h2), "size mismatch"
    n = w * h
    changed = 0
    sum_abs = 0
    max_abs = 0
    minx, miny, maxx, maxy = w, h, -1, -1
    hist = [0] * 5
    for i in range(n):
        o = i * 3
        d = max(abs(a[o] - b[o]), abs(a[o + 1] - b[o + 1]), abs(a[o + 2] - b[o + 2]))
        if d > 0:
            changed += 1
            sum_abs += d
            max_abs = max(max_abs, d)
            x, y = i % w, i // w
            minx, maxx = min(minx, x), max(maxx, x)
            miny, maxy = min(miny, y), max(maxy, y)
            hist[min(d, 4)] += 1
    print(f"size {w}x{h}")
    print(f"changed pixels : {changed} ({100.0*changed/n:.3f}%)")
    if changed:
        print(f"mean |diff| (changed) : {sum_abs/changed:.3f}")
        print(f"max  |diff|           : {max_abs}")
        print(f"bbox (x0,y0,x1,y1)     : {minx},{miny},{maxx},{maxy}")
        print("histogram of |diff| 1..4+ :", hist[1:])
    # the mean over the whole frame: the noise floor of this comparison
    tot = 0
    for i in range(n):
        o = i * 3
        tot += abs(a[o] - b[o]) + abs(a[o + 1] - b[o + 1]) + abs(a[o + 2] - b[o + 2])
    print(f"mean |diff| (whole frame, 3ch) : {tot/(3.0*n):.4f}")


if __name__ == "__main__":
    main()
