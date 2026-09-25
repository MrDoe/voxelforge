#!/usr/bin/env python3
"""Volumetric fog regression guard for voxelforge.

`volumetric_fog.comp` (J / VF_VOLFOG) integrates Rayleigh+Mie in-scatter along
the *actual* view ray to the G-buffer hit. An earlier version marched a fixed
16 m from the camera regardless of where the surface was, so it accumulated
extinction through nearby geometry and darkened ~42 % of the hero frame below
luma 20. This guard pins the fixed behaviour:

  - the mean luminance shift is bounded (haze, not a dimming filter or a
    whiteout);
  - NO pixel is pushed below luma 20 that was not already there (the exact
    regression above);
  - distant dark geometry LIFTS (aerial perspective fades toward the sky)
    instead of sinking;
  - the sun lobe is directional: looking toward the sun adds materially more
    in-scatter than looking away (the Henyey-Greenstein phase term is live).

Usage: fog_check.py <path-to-voxelforge-binary>
Stdlib only - parses the PPM output directly.
"""
import os
import subprocess
import sys
import tempfile

SHOTS = [
    # (name, cam args) - the canonical visual_check views
    ("hero", ["-16", "6.5", "-14", "6.5", "0.8", "11"]),
    ("house", ["2.5", "1.3", "6.0", "6.8", "1.0", "12.2"]),
    ("water", ["8.5", "0.6", "8.2", "4.5", "-1.1", "6.8"]),
]

MODES = [("splat", []), ("svo", ["--mode", "svo"])]

# Sun at the default 34 deg / 238 deg: direction toward the sun. A camera at
# (0,5,0) aimed along +/- this vector looks straight at / away from the sun.
SUN_DIR = (-0.703, 0.559, -0.439)
SUN_VIEW = ["0", "5", "0", "-14.06", "16.18", "-8.78"]
AWAY_VIEW = ["0", "5", "0", "14.06", "16.18", "8.78"]

W, H = 480, 270
FAST = os.environ.get("VF_FAST_TESTS") == "1"
FLAGS = "31"  # bit0 AO, bit1 shadows, bit2 flora, bit3 water, bit4 outline


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"P6":
        raise RuntimeError("not a P6 ppm")
    idx = 2
    vals = []
    while len(vals) < 3:
        while idx < len(data) and data[idx : idx + 1].isspace():
            idx += 1
        if data[idx : idx + 1] == b"#":
            while data[idx : idx + 1] not in (b"\n", b""):
                idx += 1
            continue
        start = idx
        while idx < len(data) and not data[idx : idx + 1].isspace():
            idx += 1
        vals.append(int(data[start:idx]))
    idx += 1
    w, h, _maxv = vals
    return w, h, data[idx : idx + w * h * 3]


def render_group(binary, tmp, mode_name, mode_args, fog, views):
    """One process renders every view of a (mode, fog) group: a world load is
    ~13 s and dominates a single --shot, so the whole suite batches."""
    state = "on" if fog else "off"
    list_path = os.path.join(tmp, f"shots_{mode_name}_{state}.txt")
    paths = []
    with open(list_path, "w") as lf:
        for name, cam in views:
            p = os.path.join(tmp, f"{name}_{state}.ppm")
            paths.append(p)
            lf.write(" ".join([p, *cam]) + "\n")
    env = dict(os.environ, VF_NO_OVERLAY="1", VF_RENDER_FLAGS=FLAGS,
               VF_VOLFOG="1" if fog else "0")
    cmd = [binary, "--shotlist", list_path, "--width", str(W), "--height", str(H),
           "--animtime", "0", *mode_args]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=900, env=env)
    if r.returncode != 0 or not all(os.path.exists(p) for p in paths):
        print(r.stderr[-2000:])
        return None
    return paths


def lum(data, i):
    return (data[3 * i] + data[3 * i + 1] + data[3 * i + 2]) / 3.0


def check_case(name, mode_name, off_p, on_p, failures):
    tag = f"{name}/{mode_name}"
    w, h, off = read_ppm(off_p)
    _, _, on = read_ppm(on_p)
    n = w * h

    off_sum = on_sum = 0.0
    dark_off = dark_on = 0
    far_dark_off = far_dark_on = 0.0
    far_dark_n = 0
    for i in range(n):
        ol, nl = lum(off, i), lum(on, i)
        off_sum += ol
        on_sum += nl
        if ol < 20:
            dark_off += 1
        if nl < 20:
            dark_on += 1
        # "distant" = below the sky band; dark = below 70 (shadowed geometry)
        if i // w > h * 0.35 and ol < 70:
            far_dark_off += ol
            far_dark_on += nl
            far_dark_n += 1

    shift_pct = (on_sum - off_sum) / max(off_sum, 1e-6) * 100.0
    dark_frac_off = dark_off / n
    dark_frac_on = dark_on / n
    far_off = far_dark_off / max(far_dark_n, 1)
    far_on = far_dark_on / max(far_dark_n, 1)

    # 1. bounded: haze must not dim the frame or white it out
    if not (-1.0 <= shift_pct <= 12.0):
        failures.append(f"{tag}: mean luminance shift {shift_pct:+.2f}% outside -1..12%")
    # 2. the exact historical regression: no new near-black pixels
    if dark_frac_on > dark_frac_off + 0.001:
        failures.append(
            f"{tag}: fog pushed pixels below luma 20 "
            f"({dark_frac_off*100:.2f}% -> {dark_frac_on*100:.2f}%)"
        )
    # 3. aerial perspective: distant shadowed geometry fades toward the sky
    if far_dark_n > 200 and far_on < far_off - 0.5:
        failures.append(
            f"{tag}: distant dark geometry darkened ({far_off:.1f} -> {far_on:.1f}), "
            "expected a lift (aerial perspective)"
        )

    print(f"[{tag}] shift {shift_pct:+5.2f}%  dark<20 {dark_frac_off*100:.2f}%->{dark_frac_on*100:.2f}%  "
          f"far-dark {far_off:5.1f}->{far_on:5.1f} (n={far_dark_n})")


def check_sun_lobe(paths, failures):
    """The Mie phase must make the sun direction materially hazier.
    paths: {"toward": (off, on), "away": (off, on)}."""
    vals = {}
    for tag, (off_p, on_p) in paths.items():
        w, h, off = read_ppm(off_p)
        _, _, on = read_ppm(on_p)
        n = w * h
        vals[tag] = sum(lum(on, i) - lum(off, i) for i in range(n)) / n
    if vals["toward"] < vals["away"] * 1.5:
        failures.append(
            f"sun-lobe: forward scatter too weak (toward {vals['toward']:.2f} vs "
            f"away {vals['away']:.2f}, expected >= 1.5x)"
        )
    print(f"[sun-lobe] added toward {vals['toward']:.2f}  away {vals['away']:.2f}  "
          f"(x{vals['toward']/max(vals['away'],1e-6):.1f})")


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    binary = os.path.abspath(sys.argv[1])
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        # The sun-lobe views are splat-only (both backends share the fog
        # shader; the SVO pair would just repeat the cost).
        sun_views = [("sun_toward", SUN_VIEW), ("sun_away", AWAY_VIEW)]
        shots = SHOTS[:1] if FAST else SHOTS
        modes = MODES[:1] if FAST else MODES
        for mode_name, mode_args in modes:
            views = [(f"{n}_{mode_name}", cam) for n, cam in shots]
            if mode_name == "splat":
                views += sun_views
            got = {}
            for fog in (False, True):
                state = "on" if fog else "off"
                paths = render_group(binary, tmp, mode_name, mode_args, fog, views)
                if paths is None:
                    failures.append(f"{mode_name}/{state}: batch render failed")
                    break
                for (name, _), p in zip(views, paths):
                    got.setdefault(name, {})[state] = p
            for name, cam in shots:
                key = f"{name}_{mode_name}"
                if key in got and "off" in got[key] and "on" in got[key]:
                    check_case(name, mode_name, got[key]["off"], got[key]["on"], failures)
            if mode_name == "splat":
                sun = {t: (got[f"sun_{t}"]["off"], got[f"sun_{t}"]["on"])
                       for t in ("toward", "away")}
                if all(len(v) == 2 for v in sun.values()):
                    check_sun_lobe(sun, failures)
    if failures:
        for f in failures:
            print("FAIL:", f)
        return 1
    print("fog_check FAST PASSED" if FAST else "fog_check PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
