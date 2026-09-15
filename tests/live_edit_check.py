#!/usr/bin/env python3
"""Live-edit (M1-M3) regression guard for voxelforge.

Renders the close-up view several times and asserts:
  - an untouched render is sane (coverage / sky probe),
  - one live store edit (VF_TEST_EDIT) at a known terrain cell for each brush
    mode - raise (Add), carve, delete and paint - logs dirty chunks + patched
    surfels and changes a visible but bounded fraction of pixels, without
    wrecking the frame (both backends for raise; splat for the store-only
    delete/paint modes, which have no record-layer form),
  - the carve-brush hover preview (VF_TEST_BRUSH, no edit applied) tints the
    affected splats warm (splat backend only).

Usage: live_edit_check.py <path-to-voxelforge-binary>
Stdlib only - parses the PPM output directly.
"""
import os
import subprocess
import sys
import tempfile

W, H = 480, 270
# close view of the edited terrain cell (432,509,452): the dome fills a
# meaningful part of the frame in both backends
CAM = ["-5.5", "1.2", "-3.5", "-8", "0.4", "-6"]
# terrain surface cell in front of the hero camera (world ~(-8, -0.15, -6))
CELL = "432,509,452"
EDIT = CELL + ",raise"


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


def stats(w, h, raw):
    n = w * h
    obj = 0
    black = 0
    top_blue = 0
    top_n = 0
    for i in range(n):
        r, g, b = raw[3 * i], raw[3 * i + 1], raw[3 * i + 2]
        is_sky = b > r + 12 and g > r + 4 and b > 120
        if not is_sky:
            obj += 1
            if (r + g + b) / 3.0 < 30:
                black += 1
        if i < w * (h // 8):
            top_n += 1
            if b >= r:
                top_blue += 1
    return {
        "obj": obj / n,
        "black": black / max(obj, 1),
        "sky_ok": top_blue / max(top_n, 1) > 0.5,
    }


def diff_stats(wa, ha, a, wb, hb, b, thresh=10):
    """(fraction of differing pixels, R sum, B sum) - the colour sums cover only
    pixels that got brighter, so a warm tint reads as R > B."""
    if (wa, ha) != (wb, hb):
        raise RuntimeError("frame size mismatch")
    n = wa * ha
    d = 0
    warm_r = warm_b = 0
    for k in range(n):
        m = max(
            abs(a[3 * k] - b[3 * k]),
            abs(a[3 * k + 1] - b[3 * k + 1]),
            abs(a[3 * k + 2] - b[3 * k + 2]),
        )
        if m > thresh:
            d += 1
            if (b[3 * k] + b[3 * k + 1] + b[3 * k + 2]) > (
                    a[3 * k] + a[3 * k + 1] + a[3 * k + 2]):
                warm_r += b[3 * k]
                warm_b += b[3 * k + 2]
    return d / n, warm_r, warm_b


def render(binary, out, extra_env=None, mode=None):
    env = dict(os.environ)
    # hermetic: skip restoring a saved live-edit overlay so the comparison
    # isolates this run's VF_TEST_EDIT (a session's painted edits would
    # otherwise dominate both frames)
    env["VF_NO_OVERLAY"] = "1"
    if extra_env:
        env.update(extra_env)
    cmd = [
        binary, "--shot", out,
        "--width", str(W), "--height", str(H),
        "--cam", *CAM,
    ]
    if mode:
        cmd += ["--mode", mode]
    return subprocess.run(cmd, capture_output=True, text=True, timeout=300, env=env)


def check_preview(binary, tmp, failures):
    """Carve-brush hover preview: VF_TEST_BRUSH activates the edit tool with a
    brush volume but applies NO edit, so the only frame difference is the tint
    over the splats the brush would affect (splat backend only)."""
    base = os.path.join(tmp, "splat_base.ppm")
    prev = os.path.join(tmp, "preview.ppm")
    r0 = None if os.path.exists(base) else render(binary, base, mode=None)
    if r0 is not None and (r0.returncode != 0 or not os.path.exists(base)):
        failures.append("preview: baseline render failed")
        return
    r1 = render(binary, prev, {"VF_TEST_BRUSH": f"{CELL},carve",
                               "VF_EDIT_DIAM": "1.5"}, mode=None)
    if r1.returncode != 0 or not os.path.exists(prev):
        failures.append("preview: render failed")
        return
    logs = (r1.stdout or "") + (r1.stderr or "")
    if "VF_TEST_BRUSH" not in logs:
        failures.append("preview: brush hook never ran")
    w, h, a = read_ppm(base)
    _, _, b = read_ppm(prev)
    d, warm_r, warm_b = diff_stats(w, h, a, w, h, b)
    print(f"[preview] carve tint on hover: pixel diff {d*100:.2f}%  warm R/B "
          f"{warm_r}/{warm_b}")
    if d < 0.0005:
        failures.append(f"preview: no visible highlight ({d*100:.3f}%)")
    if d > 0.25:
        failures.append(f"preview: highlight covers too much ({d*100:.1f}%)")
    if not (warm_r > warm_b and warm_r > 0):
        failures.append("preview: tinted pixels are not warm (carve tint)")


def check_pair(binary, tmp, tag, mode, failures, min_diff=0.02, max_diff=0.60,
               max_black=0.05, edit=EDIT, base_name=None, extra_env=None):
    # the untouched frame is shared between all checks on the same backend
    base = os.path.join(tmp, base_name or f"{tag}_base.ppm")
    editp = os.path.join(tmp, f"{tag}_edit.ppm")
    if os.path.exists(base):
        r0 = None
    else:
        r0 = render(binary, base, extra_env, mode=mode)
    if r0 is not None and (r0.returncode != 0 or not os.path.exists(base)):
        failures.append(f"{tag}: baseline render failed")
        return
    env = dict(extra_env) if extra_env else {}
    env["VF_TEST_EDIT"] = edit
    r1 = render(binary, editp, env, mode=mode)
    if r1.returncode != 0 or not os.path.exists(editp):
        failures.append(f"{tag}: edited render failed")
        return
    logs = (r1.stdout or "") + (r1.stderr or "")
    if "live edit:" not in logs:
        failures.append(f"{tag}: edited run never logged 'live edit:'")
    elif "surfels" not in logs:
        failures.append(f"{tag}: live edit log missing surfel count")

    w, h, a = read_ppm(base)
    _, _, b = read_ppm(editp)
    sb = stats(w, h, a)
    se = stats(w, h, b)
    d = diff_stats(w, h, a, w, h, b)[0]
    print(f"[{tag}] baseline coverage {sb['obj']*100:.1f}%  "
          f"edited coverage {se['obj']*100:.1f}%  pixel diff {d*100:.2f}%")

    if d < min_diff:
        failures.append(f"{tag}: edit too small ({d*100:.2f}% pixels changed)")
    if d > max_diff:
        failures.append(f"{tag}: edit changed too much ({d*100:.2f}%)")
    if not (0.03 <= se["obj"] <= 0.985):
        failures.append(f"{tag}: edited coverage out of range: {se['obj']:.3f}")
    if se["black"] > max_black:
        failures.append(
            f"{tag}: edited black-in-silhouette {se['black']*100:.2f}% > {max_black*100:.0f}%")

    if not se["sky_ok"]:
        failures.append(f"{tag}: edited sky probe not blue-dominant")


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    binary = os.path.abspath(sys.argv[1])
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        # splat (default) and the SVO reference both patch the same edit
        check_pair(binary, tmp, "splat", None, failures, min_diff=0.05, max_diff=0.70,
                   max_black=0.09, base_name="splat_base.ppm")
        check_pair(binary, tmp, "svo", "svo", failures, min_diff=0.03, max_diff=0.70,
                   max_black=0.09, base_name="svo_base.ppm")
        # store-only brush modes (no record-layer equivalent): delete clears the
        # brush ball, paint recolours it. Micro-detail off so the diff is the
        # geometry itself, not the patched chunks' dropped micro tail.
        no_micro = {"VF_MICRO": "0"}
        check_pair(binary, tmp, "delete", None, failures, min_diff=0.02, max_diff=0.70,
                   max_black=0.09, edit=CELL + ",delete",
                   base_name="splat_nomicro_base.ppm", extra_env=no_micro)
        check_pair(binary, tmp, "paint", None, failures, min_diff=0.02, max_diff=0.70,
                   max_black=0.09, edit=CELL + ",paint",
                   base_name="splat_nomicro_base.ppm", extra_env=no_micro)
        # carve-brush hover preview (tint only, no edit)
        check_preview(binary, tmp, failures)

    if failures:
        for f in failures:
            print("FAIL:", f)
        return 1
    print("live_edit_check PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
