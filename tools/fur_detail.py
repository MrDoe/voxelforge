#!/usr/bin/env python3
"""The coat's two fine-detail metrics, over a region of a PPM render.

  detail  - the mean |difference| between HORIZONTAL NEIGHBOURS. A coat that
            is a FLAT WASH (blurry) has almost none; a coat that is CRUNCHY
            (spiky) has a lot. The bare body sits between the two.
  spread  - the standard deviation of the luma. A flat wash has a LOW
            spread (nothing but the body's colour survives); a coat that keeps
            the fleece's texture has a HIGH one. This is the metric that
            says "the coat still shows the wool".

Usage: fur_detail.py FILE.ppm [x0 y0 x1 y1]
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
    if len(sys.argv) > 5:
        x0, y0, x1, y1 = (int(v) for v in sys.argv[2:6])
    else:
        x0, y0, x1, y1 = 260, 100, 520, 480
    w, h, buf = read_ppm(path)
    tot = 0
    cnt = 0
    lum = []
    for y in range(y0, min(y1, h)):
        row = y * w
        for x in range(x0, min(x1, w) - 1):
            o = (row + x) * 3
            for c in range(3):
                tot += abs(buf[o + c] - buf[o + c + 3])
            cnt += 3
    for y in range(y0, min(y1, h)):
        row = y * w
        for x in range(x0, min(x1, w)):
            o = (row + x) * 3
            lum.append((buf[o] * 299 + buf[o + 1] * 587 + buf[o + 2] * 114) / 1000.0)
    mean = sum(lum) / max(len(lum), 1)
    var = sum((v - mean) ** 2 for v in lum) / max(len(lum), 1)
    print(f"{path}: detail {tot/max(cnt,1):6.2f}   luma spread {var**0.5:6.2f}   mean {mean:6.2f}")


if __name__ == "__main__":
    main()
