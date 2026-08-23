#!/usr/bin/env python3
"""Visual regression guard for voxelforge.

Renders canonical headless shots and asserts structural sanity:
  - geometry coverage within plausible bounds
  - near-black pixel fraction INSIDE the object silhouette < 5%
    (this is the check that would have caught the hollow-voxel bug)
  - sky probe stays blue-dominant

Usage: visual_check.py <path-to-voxelforge-binary>
Stdlib only - parses the PPM output directly.
"""
import subprocess
import sys
import tempfile
import os
import struct

SHOTS = [
    # (name, cam args)
    ("hero", ["-16", "6.5", "-14", "6.5", "0.8", "11"]),
    ("house", ["2.5", "1.3", "6.0", "6.8", "1.0", "12.2"]),
    ("water", ["8.5", "0.6", "8.2", "4.5", "-1.1", "6.8"]),
]

W, H = 480, 270


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # P6 header: magic, whitespace, width, height, maxval, single whitespace, raw
    if data[:2] != b"P6":
        raise RuntimeError("not a P6 ppm")
    idx = 2
    vals = []
    while len(vals) < 3:
        while idx < len(data) and data[idx : idx + 1].isspace():
            idx += 1
        if data[idx:idx+1] == b"#":
            while data[idx:idx+1] not in (b"\n", b""):
                idx += 1
            continue
        start = idx
        while idx < len(data) and not data[idx : idx + 1].isspace():
            idx += 1
        vals.append(int(data[start:idx]))
    idx += 1
    w, h, _maxv = vals
    px = data[idx : idx + w * h * 3]
    return w, h, px


def analyze(path):
    w, h, raw = read_ppm(path)
    n = w * h
    sky = [False] * n
    dark_in_obj = 0
    obj_count = 0
    black_in_obj = 0
    top_strip_blue = 0
    top_strip_n = 0
    for i in range(n):
        r, g, b = raw[3 * i], raw[3 * i + 1], raw[3 * i + 2]
        is_sky = b > r + 12 and g > r + 4 and b > 120
        sky[i] = is_sky
        if not is_sky:
            obj_count += 1
            lum = (r + g + b) / 3.0
            if lum < 30:
                black_in_obj += 1
        if i < w * (h // 8):
            top_strip_n += 1
            if b >= r:
                top_strip_blue += 1
    return {
        "obj_frac": obj_count / n,
        "black_in_obj": black_in_obj / max(obj_count, 1),
        "sky_probe_ok": top_strip_blue / max(top_strip_n, 1) > 0.5,
    }


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    binary = os.path.abspath(sys.argv[1])
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        for name, cam in SHOTS:
            out = os.path.join(tmp, name + ".ppm")
            cmd = [
                binary, "--shot", out, "--backend", "svo",
                "--width", str(W), "--height", str(H),
                "--cam", *cam,
            ]
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
            if r.returncode != 0 or not os.path.exists(out):
                failures.append(f"{name}: render failed rc={r.returncode}")
                continue
            m = analyze(out)
            print(
                f"[{name}] coverage {m['obj_frac']*100:.1f}%  "
                f"black-in-silhouette {m['black_in_obj']*100:.2f}%  "
                f"sky-probe {'ok' if m['sky_probe_ok'] else 'BAD'}"
            )
            if not (0.03 <= m["obj_frac"] <= 0.97):
                failures.append(f"{name}: coverage {m['obj_frac']:.3f} out of range")
            if m["black_in_obj"] > 0.05:
                failures.append(
                    f"{name}: black-in-silhouette {m['black_in_obj']*100:.2f}% > 5%"
                )
            if not m["sky_probe_ok"]:
                failures.append(f"{name}: sky probe not blue-dominant")
    if failures:
        for f in failures:
            print("FAIL:", f)
        return 1
    print("visual_check PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
