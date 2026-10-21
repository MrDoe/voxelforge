#!/usr/bin/env python3
"""A high-frequency noise metric for a region of a PPM render.

A vision caption cannot judge speckle, so the instrument is numeric: the
mean absolute difference between HORIZONTAL NEIGHBOURS in a region. A smooth
surface has a low value; a speckled one (sub-pixel cards landing on the pixel
grid at random) has a high one.

Usage: hf_noise.py FILE.ppm x0 y0 x1 y1
"""
import sys


def read_ppm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        w, h = map(int, f.readline().split())
        f.readline()
        return w, h, f.read()


def main():
    path = sys.argv[1]
    x0, y0, x1, y1 = (int(v) for v in sys.argv[2:6])
    w, h, buf = read_ppm(path)
    tot = 0
    cnt = 0
    for y in range(y0, min(y1, h)):
        row = y * w
        for x in range(x0, min(x1, w) - 1):
            o = (row + x) * 3
            o2 = o + 3
            for c in range(3):
                tot += abs(buf[o + c] - buf[o2 + c])
            cnt += 3
    print(f"{path}: mean |neighbour diff| over {cnt} samples = {tot/max(cnt,1):.4f}")


if __name__ == "__main__":
    main()
