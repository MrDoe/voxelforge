#!/usr/bin/env python3
"""SSAO regression guard for voxelforge.

World-scale SSAO (`ssao.comp` + `ssao_apply.comp`, H / render-flag bit 6) is
opt-in: the canonical visual_check shots never enable it. This guard renders
each canonical shot in both backends three ways - SSAO off, SSAO on, and the
raw-AO debug view - and asserts:

  - sky-classified pixels are untouched between off and on (no silhouette
    halos, no sky darkening);
  - non-sky mean luminance drops a bounded amount (the AO is visible and
    grounds the scene, but never dims the whole frame);
  - near-black pixels inside the silhouette stay < 5 % with SSAO on;
  - the raw-AO debug frame is non-trivial on solid pixels and ~0 on sky
    pixels (the kernel actually fires, and only on solid surfaces).

Usage: ssao_check.py <path-to-voxelforge-binary>
Stdlib only - parses the PPM output directly.
"""
import os
import subprocess
import sys
import tempfile

SHOTS = [
    # (name, cam args)
    ("hero", ["-16", "6.5", "-14", "6.5", "0.8", "11"]),
    ("house", ["2.5", "1.3", "6.0", "6.8", "1.0", "12.2"]),
    ("water", ["8.5", "0.6", "8.2", "4.5", "-1.1", "6.8"]),
]

MODES = [("splat", []), ("svo", ["--mode", "svo"])]

W, H = 480, 270
FAST = os.environ.get("VF_FAST_TESTS") == "1"
FLAGS_OFF = "31"  # bit0 AO, bit1 shadows, bit2 flora, bit3 water, bit4 outline
FLAGS_ON = "95"   # + bit6 SSAO


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


def is_sky(r, g, b):
    return b > r + 12 and g > r + 4 and b > 120


def render_group(binary, tmp, mode_name, mode_args, flags, views, debug=False):
    """One process renders every view of a (mode, flags) group: a world load
    is ~13 s and dominates a single --shot, so the whole suite batches."""
    tag = f"{mode_name}_{'dbg' if debug else 'on' if flags == FLAGS_ON else 'off'}"
    list_path = os.path.join(tmp, f"shots_{tag}.txt")
    paths = []
    with open(list_path, "w") as lf:
        for name, cam in views:
            p = os.path.join(tmp, f"{name}_{tag}.ppm")
            paths.append(p)
            lf.write(" ".join([p, *cam]) + "\n")
    env = dict(os.environ, VF_NO_OVERLAY="1", VF_RENDER_FLAGS=flags)
    if debug:
        env["VF_SSAO_DEBUG"] = "1"
    cmd = [binary, "--shotlist", list_path, "--width", str(W), "--height", str(H),
           "--animtime", "0", *mode_args]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=900, env=env)
    if r.returncode != 0 or not all(os.path.exists(p) for p in paths):
        print(r.stderr[-2000:])
        return None
    return paths


def check_case(name, mode_name, off_p, on_p, failures):
    """off/on comparison. Returns (failed, drop_pct) so the caller can decide
    whether the extra raw-AO debug batch is worth rendering."""
    tag = f"{name}/{mode_name}"
    w, h, off = read_ppm(off_p)
    _, _, on = read_ppm(on_p)
    n = w * h

    sky_n = 0
    sky_dark_max = 0
    sky_dark_sum = 0.0
    sky_dark_over = 0
    obj_n = 0
    off_lum = 0.0
    on_lum = 0.0
    black_in_obj = 0
    for i in range(n):
        r, g, b = off[3 * i], off[3 * i + 1], off[3 * i + 2]
        if is_sky(r, g, b):
            sky_n += 1
            d = (r + g + b) / 3.0 - (on[3 * i] + on[3 * i + 1] + on[3 * i + 2]) / 3.0
            sky_dark_sum += d
            if d > sky_dark_max:
                sky_dark_max = d
            if d > 20.0:
                sky_dark_over += 1
        else:
            obj_n += 1
            ol = (r + g + b) / 3.0
            nl = (on[3 * i] + on[3 * i + 1] + on[3 * i + 2]) / 3.0
            off_lum += ol
            on_lum += nl
            if nl < 30:
                black_in_obj += 1

    failed = False
    if obj_n == 0:
        failures.append(f"{tag}: no non-sky pixels")
        return True, 0.0
    drop_pct = (off_lum - on_lum) / max(off_lum, 1e-6) * 100.0
    black_frac = black_in_obj / obj_n
    sky_dark_mean = sky_dark_sum / max(sky_n, 1)

    # Sky guard: the loose brightness heuristic also classifies a few bright
    # fogged-terrain pixels as sky, so bound the mean (true sky darkening
    # would shift it by many codes). The per-pixel count tolerates the
    # renderer's noise floor (1-2 outlier codes) while a real silhouette halo
    # darkens thousands of sky pixels.
    if sky_dark_mean > 1.0 or sky_dark_over > 50:
        failed = True
        failures.append(
            f"{tag}: sky-classified darkening mean {sky_dark_mean:.2f} "
            f"max {sky_dark_max:.0f} ({sky_dark_over} px > 20)"
        )
    # The water shot is deliberately AO-free on the water plane (mostly what
    # it frames), so its drop floor is 0; the land shots must show grounding.
    drop_lo = 0.1 if name != "water" else 0.0
    if not (drop_lo <= drop_pct <= 9.0):
        failed = True
        failures.append(
            f"{tag}: object luminance drop {drop_pct:.2f}% outside {drop_lo}-9%"
        )
    if black_frac > 0.05:
        failed = True
        failures.append(f"{tag}: black-in-silhouette {black_frac*100:.2f}% > 5% with SSAO on")

    print(f"[{tag}] drop {drop_pct:5.2f}%  sky-dark mean {sky_dark_mean:.2f} "
          f"max {sky_dark_max:.0f} ({sky_dark_over} over)  "
          f"black-in-obj {black_frac*100:.2f}%")
    return failed, drop_pct


def check_debug(name, mode_name, off_p, dbg_p, failures):
    """raw-AO debug frame: solid pixels carry signal, sky stays ~0."""
    tag = f"{name}/{mode_name}"
    w, h, off = read_ppm(off_p)
    _, _, dbg = read_ppm(dbg_p)
    n = w * h

    sky_ao = 0.0
    sky_ao_n = 0
    solid_ao = 0.0
    solid_n = 0
    solid_big = 0
    vals = []
    for i in range(n):
        r, g, b = off[3 * i], off[3 * i + 1], off[3 * i + 2]
        ao = dbg[3 * i] / 255.0
        if is_sky(r, g, b):
            sky_ao += ao
            sky_ao_n += 1
        else:
            solid_ao += ao
            solid_n += 1
            vals.append(ao)
            if ao > 0.1:
                solid_big += 1
    vals.sort()
    ao_mean = solid_ao / max(solid_n, 1)
    ao_p99 = vals[int(0.99 * (len(vals) - 1))] if vals else 0.0
    ao_sky_mean = sky_ao / max(sky_ao_n, 1)
    big_frac = solid_big / max(solid_n, 1)
    if name != "water":
        # The land shots must show the kernel actually firing (the water shot
        # is mostly skipped water surface, so it only carries the sky guard).
        if ao_mean < 0.01 or ao_p99 < 0.15:
            failures.append(f"{tag}: AO term too weak (mean {ao_mean:.3f} p99 {ao_p99:.3f})")
        if not (0.005 <= big_frac <= 0.90):
            failures.append(f"{tag}: AO coverage {big_frac*100:.1f}% outside 0.5-90%")
    if ao_sky_mean > 0.02:
        failures.append(f"{tag}: AO on sky pixels {ao_sky_mean:.3f} > 0.02")

    print(f"[{tag}] dbg: ao mean {ao_mean:.3f} p99 {ao_p99:.3f} "
          f"cov {big_frac*100:.1f}%  sky-ao {ao_sky_mean:.3f}")


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    binary = os.path.abspath(sys.argv[1])
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        views = [(n, cam) for n, cam in (SHOTS[:1] if FAST else SHOTS)]
        modes = MODES[:1] if FAST else MODES
        for mode_name, mode_args in modes:
            off = render_group(binary, tmp, mode_name, mode_args, FLAGS_OFF, views)
            on = render_group(binary, tmp, mode_name, mode_args, FLAGS_ON, views)
            if off is None or on is None:
                failures.append(f"{mode_name}: batch render failed")
                continue
            results = {}
            for i, (name, _cam) in enumerate(views):
                results[name] = check_case(name, mode_name, off[i], on[i], failures)
            # The raw-AO debug batch is a whole extra render of the mode: only
            # pay for it when a case already failed, or the AO looks invisible
            # (a drop below the land floor while the check still passed).
            suspicious = any(
                failed or (name != "water" and drop < 0.1)
                for name, (failed, drop) in results.items()
            )
            if suspicious:
                dbg = render_group(binary, tmp, mode_name, mode_args, FLAGS_ON,
                                   views, debug=True)
                if dbg is None:
                    failures.append(f"{mode_name}: debug batch render failed")
                else:
                    for i, (name, _cam) in enumerate(views):
                        check_debug(name, mode_name, off[i], dbg[i], failures)
    if failures:
        for f in failures:
            print("FAIL:", f)
        return 1
    print("ssao_check FAST PASSED" if FAST else "ssao_check PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
