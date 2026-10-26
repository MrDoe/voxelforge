#!/usr/bin/env python3
"""Numeric masks for micro-object deformation defects (holes / ghosts / black).

The point of this tool is to make a visual claim MEASURABLE, because a vision
caption of a render is far too coarse to judge a black notch a few pixels wide
(AGENTS.md: "Screenshots and metrics beat descriptions").

Metrics, all computed from PPM (P6) images:

  grid    <img>                coarse luma map, to confirm the framing
  black   <img> [--thresh N]   near-black pixels that sit INSIDE the object
  diff    <a.ppm> <b.ppm>      changed region, split by what b changed it TO
  holes   <obj.ppm> <plate.ppm> object pixels that show the plate through them

`plate` is the SAME camera with the micro object hidden - see
`--plate-manifest`, which builds an isolated asset dir of symlinks plus a
manifest copy, so the shipped assets/world.json is never rewritten (the gate
in tests/micro_check.py rewrites and restores it, which AGENTS.md records as
a data-loss path).

Usage examples:
  tools/micro_deform_metrics.py grid  /tmp/opencode/graze_peak.ppm
  tools/micro_deform_metrics.py black /tmp/opencode/graze_peak.ppm --thresh 12
  tools/micro_deform_metrics.py diff  /tmp/opencode/before.ppm /tmp/opencode/after.ppm
  tools/micro_deform_metrics.py holes /tmp/opencode/pose.ppm /tmp/opencode/plate.ppm
"""
import argparse
import sys


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"P6":
        raise SystemExit("%s: not a binary PPM (P6)" % path)
    vals, i = [], 2
    while len(vals) < 3:
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] not in (b"", b"\n"):
                i += 1
            continue
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        vals.append(int(data[i:j]))
        i = j
    i += 1
    w, h, _ = vals
    return w, h, data[i:i + w * h * 3]


def lumas(w, h, px):
    out = [0.0] * (w * h)
    for k in range(w * h):
        o = k * 3
        out[k] = 0.2126 * px[o] + 0.7152 * px[o + 1] + 0.0722 * px[o + 2]
    return out


def dilate(mask, w, h, r=1):
    out = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            if not mask[y * w + x]:
                continue
            for dy in range(-r, r + 1):
                yy = y + dy
                if yy < 0 or yy >= h:
                    continue
                for dx in range(-r, r + 1):
                    xx = x + dx
                    if 0 <= xx < w:
                        out[yy * w + xx] = 1
    return out


def bbox(w, h, mask):
    xs = [i % w for i in range(w * h) if mask[i]]
    ys = [i // w for i in range(w * h) if mask[i]]
    if not xs:
        return None
    return min(xs), min(ys), max(xs), max(ys)


def cmd_grid(args):
    w, h, px = read_ppm(args.img)
    lum = lumas(w, h, px)
    cols, rows = args.cols, args.rows
    ramp = " .:-=+*#%@"
    print("grid %dx%d of %dx%d" % (cols, rows, w, h))
    for gy in range(rows):
        line = []
        for gx in range(cols):
            x0, x1 = gx * w // cols, (gx + 1) * w // cols
            y0, y1 = gy * h // rows, (gy + 1) * h // rows
            s, n = 0.0, 0
            for y in range(y0, max(y1, y0 + 1)):
                for x in range(x0, max(x1, x0 + 1)):
                    s += lum[y * w + x]
                    n += 1
            v = s / max(n, 1)
            line.append(ramp[min(len(ramp) - 1, int(v * len(ramp) / 256))])
        print("  " + "".join(line))
    lo = min(lum)
    hi = max(lum)
    print("luma min %.1f max %.1f mean %.1f" % (lo, hi, sum(lum) / len(lum)))


def cmd_black(args):
    w, h, px = read_ppm(args.img)
    lum = lumas(w, h, px)
    dark = bytearray(1 if v < args.thresh else 0 for v in lum)
    bright = bytearray(1 if v > args.bright else 0 for v in lum)
    # A defect pixel is near-black AND has bright pixels around it: the notch
    # is a hole in a lit coat, so its neighbourhood proves the location is not
    # simply unlit background.
    bd = dilate(bright, w, h, r=args.radius)
    defect = bytearray(1 if (dark[i] and bd[i]) else 0 for i in range(w * h))
    nd, nbad = sum(dark), sum(defect)
    print("%s: %dx%d" % (args.img, w, h))
    print("near-black (<%d): %d px (%.3f%%)"
          % (args.thresh, nd, 100.0 * nd / (w * h)))
    print("near-black with bright within %d px: %d px" % (args.radius, nbad))
    bb = bbox(w, h, defect)
    print("defect bbox:", bb)
    if args.rows and bb:
        x0, y0, x1, y1 = bb
        print("defect crop %d,%d..%d,%d (w=%d h=%d)"
              % (x0, y0, x1, y1, x1 - x0 + 1, y1 - y0 + 1))
    return 0 if nbad == 0 else 1


def cmd_diff(args):
    wa, ha, pa = read_ppm(args.a)
    wb, hb, pb = read_ppm(args.b)
    if (wa, ha) != (wb, hb):
        raise SystemExit("size mismatch: %dx%d vs %dx%d" % (wa, ha, wb, hb))
    w, h = wa, ha
    la, lb = lumas(w, h, pa), lumas(w, h, pb)
    changed = bytearray(w * h)
    to_black = to_coat = 0
    total = 0
    for i in range(w * h):
        if abs(la[i] - lb[i]) <= args.thresh:
            continue
        changed[i] = 1
        total += 1
        if lb[i] < args.black:
            to_black += 1
        elif la[i] < args.black:
            to_coat += 1
    print("A=%s  B=%s" % (args.a, args.b))
    print("changed (>%d luma): %d px (%.3f%%)"
          % (args.thresh, total, 100.0 * total / (w * h)))
    print("  became near-black (<%d): %d" % (args.black, to_black))
    print("  was near-black, now lit: %d" % to_coat)
    bb = bbox(w, h, changed)
    print("changed bbox:", bb)
    return 0


def cmd_holes(args):
    wo, ho, po = read_ppm(args.obj)
    wp, hp, pp = read_ppm(args.plate)
    if (wo, ho) != (wp, hp):
        raise SystemExit("size mismatch")
    w, h = wo, ho
    lo_, lp_ = lumas(w, h, po), lumas(w, h, pp)
    obj = bytearray(1 if abs(lo_[i] - lp_[i]) > args.thresh else 0 for i in range(w * h))
    # Erode the silhouette so the rim (where a splat half-covers a pixel) does
    # not read as a hole.
    for _ in range(args.erode):
        inv = bytearray(1 - v for v in obj)
        d = dilate(inv, w, h, r=1)
        obj = bytearray(1 if (obj[i] and not d[i]) else 0 for i in range(w * h))
    holes = bytearray(
        1 if (obj[i] and abs(lo_[i] - lp_[i]) <= args.thresh) else 0
        for i in range(w * h))
    black = bytearray(1 if (obj[i] and lo_[i] < args.black) else 0 for i in range(w * h))
    n = sum(obj)
    print("object silhouette (eroded %d): %d px" % (args.erode, n))
    print("core holes (background shows through): %d px" % sum(holes))
    print("near-black inside silhouette (<%d): %d px" % (args.black, sum(black)))
    print("holes bbox:", bbox(w, h, holes))
    print("black bbox:", bbox(w, h, black))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    g = sub.add_parser("grid")
    g.add_argument("img")
    g.add_argument("--cols", type=int, default=48)
    g.add_argument("--rows", type=int, default=24)
    g.set_defaults(fn=cmd_grid)

    b = sub.add_parser("black")
    b.add_argument("img")
    b.add_argument("--thresh", type=float, default=12.0)
    b.add_argument("--bright", type=float, default=60.0)
    b.add_argument("--radius", type=int, default=2)
    b.add_argument("--rows", action="store_true")
    b.set_defaults(fn=cmd_black)

    d = sub.add_parser("diff")
    d.add_argument("a")
    d.add_argument("b")
    d.add_argument("--thresh", type=float, default=8.0)
    d.add_argument("--black", type=float, default=12.0)
    d.set_defaults(fn=cmd_diff)

    h = sub.add_parser("holes")
    h.add_argument("obj")
    h.add_argument("plate")
    h.add_argument("--thresh", type=float, default=8.0)
    h.add_argument("--black", type=float, default=12.0)
    h.add_argument("--erode", type=int, default=1)
    h.set_defaults(fn=cmd_holes)

    args = ap.parse_args()
    sys.exit(args.fn(args))


if __name__ == "__main__":
    main()
