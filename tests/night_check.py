#!/usr/bin/env python3
"""Night-phase regression guard for voxelforge.

Exists because every existing gate is calibrated on daylight. `visual_check`'s
three numbers all break at night:

  - `black-in-silhouette > 0.05`    - a night frame is 66-86% below luma 30, so
                                     the gate fails on a CORRECT night render.
  - coverage, which classifies sky with `b > r+12 and g > r+4 and b > 120` -
                                     the `b > 120` term is a daylight term.
                                     MEASURED: it reports **0.00%** sky on all
                                     three night shots, while the same test
                                     without that term reports 40.1 / 8.7 / 10.0%.
                                     A correct night frame reads as "nothing
                                     rendered".
  - `sky_probe_ok` (top-eighth `b >= r`) - brightness-independent, so it passes
                                     at night without testing anything.

So this is a SEPARATE suite with its OWN thresholds, modelled on `fog_check`'s
off/on structure: one process per sun state (a world load is ~17.6 s and
dominates), so the default gate run pays two extra loads, not six. It is
deliberately not bolted onto `visual_check`'s SHOTS.

It pins the **night preset** (-30/-96 via `VF_TEST_SUN_PHASE=night`), not the
clock's 00:00 (-60/0) and not "night" generically. The tree has three reachable
nights and they are not the same frame; `tests/test_app_cli_ui.cpp` asserts the
distinction deterministically on the angles, which is why it is not asserted on
luma here -- the two measured arms differ by 1.7 mean luma, inside the 1.3
spread that made the raw means unusable.

THRESHOLDS ARE RATIOS TO THE DAY ARM AT THE SAME CAMERA, on purpose. Absolute
mean luma is camera-dependent to a degree that decides this gate: the same
preset and the same backend measure 22.96 at the `visual_check` hero camera and
31.63 at the reference camera, a 37% gap from framing alone. So the gate asserts
ratios and per-shot floors, and the luma ceiling is expressed as a ratio too.

Usage: night_check.py <path-to-voxelforge-binary>
Stdlib only - parses the PPM output directly.
"""
import os
import subprocess
import sys
import tempfile

# The canonical visual_check views, at the resolution visual_check uses.
SHOTS = [
    ("hero", ["-16", "6.5", "-14", "6.5", "0.8", "11"]),
    ("house", ["2.5", "1.3", "6.0", "6.8", "1.0", "12.2"]),
    ("water", ["8.5", "0.6", "8.2", "4.5", "-1.1", "6.8"]),
]

W, H = 480, 270
FAST = os.environ.get("VF_FAST_TESTS") == "1"
FLAGS = "31"  # bit0 AO, bit1 shadows, bit2 flora, bit3 water, bit4 outline

# Reference (measured 2026-10-08 on the 169-light flip tree, 480x270 splat,
# VF_RENDER_FLAGS=31, VF_TEST_SUN_PHASE=night, overlay suppressed;
# 2 authored courtyard + 167 derived incl. ~150 underwater lava emitters).
# Per-shot night/day mean-luma ratio: hero 0.204, house 0.200, water 0.368.
RATIO_FLOOR = 0.10   # night must actually be night
# Tripwire: a kMoonCol regression from 0.14 to 0.62. RE-MEASURED in the
# 169-light state on 2026-10-08 (temp shader bump 0.14->0.62 + SPV rebuild,
# reverted + rebuilt after; this retires the old PROVISIONAL note):
#     view    healthy   regressed   midpoint
#     hero    0.204     0.337       0.270
#     house   0.200     0.336       0.268
#     water   0.368     0.515       0.442
# One scalar ceiling cannot serve all three: the hero regression (0.337) sits
# BELOW healthy water (0.368), because lava owns the water view while the moon
# owns hero. So the ceiling is per view, each the midpoint of that view's own
# healthy/regressed pair (same symmetric-margin method the single 0.27 came
# from). Margins: hero +-0.066, house +-0.068, water +-0.073.
RATIO_CEIL = {"hero": 0.27, "house": 0.27, "water": 0.44}
MIN_SKY_PCT = 5.0   # night sky is still sky (own classifier, no b>120 term)
# Moonlight is blue-dominant in the top eighth - except where lava owns the
# strip: healthy water reads 57.0 (lava glow) and the moon regression moves it
# only to 56.8, so no moon-sensitive floor exists for that view. Per-view
# floors: hero/house keep 60 (healthy 100.0/73.4 clear it), water takes 50
# (clears healthy 57.0 with the same order of margin, still catches an empty
# frame via MIN_SKY_PCT).
MIN_TOP_BLUE = {"hero": 60.0, "house": 60.0, "water": 50.0}


def read_ppm(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:2] != b"P6":
        raise RuntimeError("not a P6 ppm: %s" % path)
    idx, vals = 2, []
    while len(vals) < 3:
        while data[idx:idx + 1].isspace():
            idx += 1
        if data[idx:idx + 1] == b"#":
            while data[idx:idx + 1] not in (b"\n", b""):
                idx += 1
            continue
        start = idx
        while not data[idx:idx + 1].isspace():
            idx += 1
        vals.append(int(data[start:idx]))
    idx += 1
    w, h, _maxv = vals
    return w, h, data[idx:idx + w * h * 3]


def render_state(binary, tmp, state, views):
    """One process renders every view of a sun state: a world load is ~17.6 s
    and dominates a single --shot, so the whole suite batches."""
    list_path = os.path.join(tmp, "shots_%s.txt" % state)
    paths = []
    with open(list_path, "w") as lf:
        for name, cam in views:
            p = os.path.join(tmp, "%s_%s.ppm" % (name, state))
            paths.append(p)
            lf.write(" ".join([p, *cam]) + "\n")
    env = dict(os.environ, VF_NO_OVERLAY="1",
               VF_OVERLAY_PATH=os.path.join(tmp, "overlay_%s.vxw" % state),
               VF_RENDER_FLAGS=FLAGS)
    if state == "night":
        env["VF_TEST_SUN_PHASE"] = "night"
    cmd = [binary, "--shotlist", list_path, "--width", str(W), "--height", str(H),
           "--animtime", "0"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=1800, env=env)
    if r.returncode != 0 or not all(os.path.exists(p) for p in paths):
        print(r.stderr[-2000:])
        return None
    return paths


def metrics(path):
    w, h, px = read_ppm(path)
    n = w * h
    top_n = w * (h // 8)
    sum_l = 0.0
    sky = top_blue = 0
    for i in range(n):
        r, g, b = px[3 * i], px[3 * i + 1], px[3 * i + 2]
        sum_l += (r + g + b) / 3.0
        # Night sky classifier: visual_check's test WITHOUT the daylight-only
        # `b > 120` term. Measured: with that term every night shot scores 0.00%
        # sky, so is_sky is false everywhere, obj_frac -> ~1.00 and the coverage
        # gate fails out of range in the opposite direction.
        if b > r + 12 and g > r + 4:
            sky += 1
        if i < top_n and b >= r:
            top_blue += 1
    mean = sum_l / n
    # RELATIVE dark share, not visual_check's absolute `lum < 30`. That constant
    # is a daylight value: the night frame's own mean is ~23, so an absolute 30
    # calls two thirds of a CORRECT night render "black in silhouette" and fails
    # the 5% gate. A share of the frame's own mean scales with the exposure.
    dark_rel = 0
    thr = 0.5 * mean
    for i in range(n):
        if (px[3 * i] + px[3 * i + 1] + px[3 * i + 2]) / 3.0 < thr:
            dark_rel += 1
    return {
        "mean": mean,
        "dark_rel": 100.0 * dark_rel / n,
        "dark_abs30": 100.0 * sum(
            1 for i in range(n)
            if (px[3 * i] + px[3 * i + 1] + px[3 * i + 2]) / 3.0 < 30) / n,
        "sky": 100.0 * sky / n,
        "top_blue": 100.0 * top_blue / max(top_n, 1),
    }


def check_case(name, day, night, failures):
    ratio = night["mean"] / max(day["mean"], 1e-6)
    # Both absolutes were always printed here; the arrow form ("a -> b") made the
    # left figure read as "the before" rather than as a DAY ANCHOR. The ratio
    # band is scale-free by design (it survives camera drift, which is the same
    # property that makes it unable to see a scene that scaled), so the day
    # absolute is the only scale-bearing quantity in this output and the only
    # place a scale change can show up. It is a REPORTED DRIFT REFERENCE, not a
    # gate: no band exists yet for it, and none may be added here alone - a
    # scale-bearing band and the ratio band must be derived in one tree in one
    # session (see concepts/night-gate-thresholds.md).
    print("[%s] mean DAY %7.2f -> NIGHT %7.2f  ratio %.3f  dark(rel<0.5x) %5.2f%%  "
          "dark(abs<30, NOT a gate) %5.2f%%  sky %5.2f%%  top-blue %5.1f%%"
          % (name, day["mean"], night["mean"], ratio, night["dark_rel"],
             night["dark_abs30"], night["sky"], night["top_blue"]))

    ceil = RATIO_CEIL[name]
    if not (RATIO_FLOOR <= ratio <= ceil):
        why = ("night is not darker than day" if ratio < RATIO_FLOOR
               else "night too bright - is kMoonCol back at 0.62? (reads as dusk; "
                    "regressed %.3f vs a %.2f ceiling at %s)"
                    % ({"hero": 0.337, "house": 0.336,
                        "water": 0.515}[name], ceil, name))
        failures.append("%s: night/day mean-luma ratio %.3f outside %.2f..%.2f (%s)"
                        % (name, ratio, RATIO_FLOOR, ceil, why))
    # A night frame must still contain sky, or "dark" and "empty" are
    # indistinguishable and the gate would pass a broken renderer.
    if night["sky"] < MIN_SKY_PCT:
        failures.append("%s: night sky only %.2f%% (floor %.1f%%) - the frame is "
                        "empty, not night" % (name, night["sky"], MIN_SKY_PCT))
    # Moonlight reads blue-dominant: the same property the daylight sky probe
    # claims to test, but here it is meaningful because the frame is dark.
    blue_floor = MIN_TOP_BLUE[name]
    if night["top_blue"] < blue_floor:
        failures.append("%s: top eighth only %.1f%% blue-dominant (floor %.1f%%) - "
                        "moonlight is not reading blue"
                        % (name, night["top_blue"], blue_floor))


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    binary = os.path.abspath(sys.argv[1])
    failures = []
    shots = SHOTS[:1] if FAST else SHOTS
    with tempfile.TemporaryDirectory() as tmp:
        got = {}
        for state in ("day", "night"):
            paths = render_state(binary, tmp, state, shots)
            if paths is None:
                print("FAIL: %s arm batch render failed" % state)
                return 1
            for (name, _), p in zip(shots, paths):
                got.setdefault(name, {})[state] = metrics(p)
        for name, _ in shots:
            if "day" in got[name] and "night" in got[name]:
                check_case(name, got[name]["day"], got[name]["night"], failures)
    if failures:
        for f in failures:
            print("FAIL:", f)
        return 1
    print("night_check FAST PASSED" if FAST else "night_check PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())