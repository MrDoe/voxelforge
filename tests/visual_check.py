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
import re
import struct
import json
import math
import time

SHOTS = [
    # (name, cam args)
    ("hero", ["-16", "6.5", "-14", "6.5", "0.8", "11"]),
    ("house", ["2.5", "1.3", "6.0", "6.8", "1.0", "12.2"]),
    ("water", ["8.5", "0.6", "8.2", "4.5", "-1.1", "6.8"]),
]

W, H = 480, 270
FAST = os.environ.get("VF_FAST_TESTS") == "1"

# --- canopy parity (the "slots in the tree" artifact) ----------------------
# The splat backend must not paint the tree canopy BLACK where the SVO
# reference lights it. The camera is the one the user reported the artifact
# from (reconstructed from the HUD line "Pos -4.2, 0.9, 14.6 / Head 187 deg
# S" plus the horizon height in the frame), and the SVO backend is the
# reference: it marches the exact geometry, the splat backend rasterises
# baked surfels.
#
# The metric is the near-black mask: pixels that are below luma 30 in the
# splat frame and at or above luma 30 in the SVO frame. A count alone is
# brittle (one dark blob moves the number), so the mean luma at the mask is
# reported next to it: 19.9 vs the reference's 60.7 on 2026-10-21.
#
# BASELINE, not a target: 21,785 px (8.51% of the frame) on 2026-10-21,
# before the footprint/shading work on the artifact. The bound is 15% above
# it, which absorbs the per-run noise (two identical renders differ by at
# most 5/255 on ~1.3% of the pixels - the lighting is not bit-exact).
# Tighten it when the artifact is fixed; never loosen it silently.
CANOPY_CAM = ["-4.2", "0.9", "14.6", "-5.404", "-0.488", "4.792"]
CANOPY_W, CANOPY_H = 640, 400
CANOPY_MASK_BASELINE = 21785
CANOPY_MASK_MAX = 24000
CANOPY_LUMA_MIN = 15.0


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


def ownership_classes(path):
    """Return 1=selected, 2=other object, 3=terrain/water ownership classes."""
    w, h, raw = read_ppm(path)
    classes = bytearray(w * h)
    for i in range(w * h):
        r, g, b = raw[3 * i], raw[3 * i + 1], raw[3 * i + 2]
        if g >= 100 and g > r * 1.7 and g > b * 1.12:
            classes[i] = 1
        elif r >= 100 and r > g * 2.0 and r > b * 2.0:
            classes[i] = 2
        elif b >= 90 and b > g * 2.0 and b > r * 3.0:
            classes[i] = 3
    return w, h, classes


def analyze_ownership(path):
    """Summarize VF_SPLAT_DEBUG=16's ownership mask."""
    _, _, classes = ownership_classes(path)
    n = len(classes)
    return {
        "selected": classes.count(1) / n,
        "other_object": classes.count(2) / n,
        "terrain_water": classes.count(3) / n,
    }


def expected_trackball(match, width, height):
    """Independently project the probe's AABB with the shader camera basis."""
    lo = tuple(float(match.group(i)) for i in range(8, 11))
    hi = tuple(float(match.group(i)) for i in range(11, 14))
    cam = tuple(float(match.group(i)) for i in range(14, 17))
    yaw = float(match.group(17))
    pitch = float(match.group(18))

    cp = math.cos(pitch)
    forward = (cp * math.cos(yaw), math.sin(pitch), cp * math.sin(yaw))
    # Camera::right() is normalize(cross(forward, worldUp)).
    right = (-forward[2], 0.0, forward[0])
    rl = math.hypot(right[0], right[2])
    right = (right[0] / rl, 0.0, right[2] / rl)
    up = (
        right[1] * forward[2] - right[2] * forward[1],
        right[2] * forward[0] - right[0] * forward[2],
        right[0] * forward[1] - right[1] * forward[0],
    )

    def dot(a, b):
        return sum(x * y for x, y in zip(a, b))

    def project(point):
        rel = tuple(point[i] - cam[i] for i in range(3))
        depth = dot(rel, forward)
        if depth <= 0.05:
            return None
        aspect = width / height
        ndc_x = dot(rel, right) / (depth * math.tan(math.radians(30.0)) * aspect)
        ndc_y = -dot(rel, up) / (depth * math.tan(math.radians(30.0)))
        return (0.5 * width * (1.0 + ndc_x),
                0.5 * height * (1.0 + ndc_y), depth)

    centre = tuple((lo[i] + hi[i]) * 0.5 for i in range(3))
    projected_centre = project(centre)
    if projected_centre is None:
        return None
    cx, cy, _ = projected_centre
    radius = 0.0
    for mask in range(8):
        corner = tuple(
            hi[i] if mask & (1 << i) else lo[i] for i in range(3)
        )
        projected_corner = project(corner)
        if projected_corner is None:
            continue
        px, py, _ = projected_corner
        radius = max(radius, math.hypot(px - cx, py - cy))
    radius = min(max(radius, 24.0), 0.46 * min(width, height))
    return cx, cy, radius


def near_black_mask(splat_path, svo_path):
    """Pixels that are near-black in the splat frame but lit in the SVO frame.

    Returns (count, mean splat luma at those pixels, mean SVO luma there).
    """
    w, h, a = read_ppm(splat_path)
    w2, h2, b = read_ppm(svo_path)
    if (w, h) != (w2, h2):
        raise RuntimeError("canopy: frame size mismatch")
    count = 0
    luma_sum = 0.0
    svo_sum = 0.0
    for i in range(w * h):
        j = 3 * i
        ls = 0.299 * a[j] + 0.587 * a[j + 1] + 0.114 * a[j + 2]
        lv = 0.299 * b[j] + 0.587 * b[j + 1] + 0.114 * b[j + 2]
        if ls < 30.0 and lv >= 30.0:
            count += 1
            luma_sum += ls
            svo_sum += lv
    if count:
        return count, luma_sum / count, svo_sum / count
    return 0, 0.0, 0.0


def check_canopy_parity(binary, tmp, env, failures):
    """The splat backend must not go black where the SVO reference is lit.

    This is the guard for the "slots/slits in the tree" artifact: the splat
    backend rasterises baked surfels and a dark inner layer could win the
    depth prepass over the lit front surface, painting a black disk where
    the SVO reference shows the real surface.
    """
    splat = os.path.join(tmp, "canopy-splat.ppm")
    svo = os.path.join(tmp, "canopy-svo.ppm")
    for out, extra in ((splat, []), (svo, ["--mode", "svo"])):
        cmd = [binary, "--shot", out, "--cam", *CANOPY_CAM,
               "--width", str(CANOPY_W), "--height", str(CANOPY_H), *extra]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=900,
                           env=env)
        if r.returncode != 0 or not os.path.exists(out):
            failures.append(f"canopy: render failed ({os.path.basename(out)})")
            return
    try:
        count, mean_luma, svo_luma = near_black_mask(splat, svo)
    except RuntimeError as exc:
        failures.append(f"canopy: {exc}")
        return
    print(
        f"[canopy] splat-only near-black {count} px "
        f"({count*100.0/(CANOPY_W*CANOPY_H):.2f}% of frame), "
        f"luma there {mean_luma:.1f} vs SVO {svo_luma:.1f} "
        f"(baseline {CANOPY_MASK_BASELINE})"
    )
    if count > CANOPY_MASK_MAX:
        failures.append(
            f"canopy: splat-only near-black {count} px > {CANOPY_MASK_MAX} "
            f"(baseline {CANOPY_MASK_BASELINE}) - the splat backend is "
            f"painting the canopy black again"
        )
    elif count > 0 and mean_luma < CANOPY_LUMA_MIN:
        failures.append(
            f"canopy: mean luma at the near-black mask {mean_luma:.1f} "
            f"< {CANOPY_LUMA_MIN} - the masked pixels are darker than the "
            f"baseline frame"
        )


def ownership_layer(manifest_path, failures):
    """Resolve the ownership subject from the manifest instead of freezing a
    content name here. A hard-coded "hamlet_cabin.vxw" silently became a stale
    fixture when the hamlet was re-authored (CabinPart1.vxw + hamlet_*): the
    app got layerId 0 for it, so the trackball never armed and the section
    failed with five misleading messages (probe not emitted, no red, nothing
    moved) instead of one.

    Key on `role == "object"`, not on a filename: role is the stable contract
    (landscape layers carry role "landscape"), and most hamlet_* layers are
    disabled at any time, so a "hamlet_*" or "landscape.vxw" name test breaks
    again on the next re-author. The single name excluded is `ai_edits.vxw` -
    the app's own live-edit layer (EditableWorld::kFileName), which the
    selection logic in App::applyEditLive also excludes because it is not a
    rotatable object. Pick the first enabled object layer: a compact one with a
    real AABB is what the trackball geometry wants."""
    try:
        with open(manifest_path, "r") as manifest_file:
            manifest = json.load(manifest_file)
    except (OSError, ValueError) as exc:
        failures.append(f"layer ownership: cannot read {manifest_path}: {exc}")
        return None
    layers = manifest.get("layers", manifest) if isinstance(manifest, dict) else manifest
    for layer in layers:
        name = layer.get("file", "")
        if (layer.get("enabled", True) and layer.get("role") == "object"
                and isinstance(name, str) and name.endswith(".vxw")
                and name != "ai_edits.vxw"):
            return name
    failures.append(
        "layer ownership: no enabled object layer in world.json to select")
    return None


def check_present_probe(binary, tmp, env, failures):
    """The acquire/present result probe must FIRE and must fire once.

    This is a diagnostic for the failure that is otherwise invisible - a window
    that goes dark, an app that spins without drawing - and until now it had no
    positive firing test, so "it logs" was a claim nobody could check. The
    forced-result hook makes it testable WITHOUT touching the real swapchain:
    the hook feeds synthetic VkResults to the reporter, and the thing under test
    is the logger, not the present.

    Two properties are asserted, and the second is the one that was actually
    broken. (1) Severity: device-lost and out-of-host-memory are errors, the
    rest are warnings. (2) Log-once PER DISTINCT CODE. The latch remembered only
    the last code, so two failures alternating logged EVERY frame - measured, 30
    frames produced 60 lines - which is the flood the latch exists to prevent.
    It hid because a real persistent failure repeats one code and behaves. The
    repeated code in the list below is what makes that property observable.

    Uses --smoke rather than --shot for ONE reason, and it is not reachability:
    the probe is reachable from headless BECAUSE forceFrameResults() is called
    from the headless frame body (--shot shares that path and reaches it too -
    measured). --smoke simply runs enough frames for "once per distinct code" to
    MEAN something; over --shot's three frames the latch would pass even if it
    were broken. The reachability fact is why the hook has that second call
    site at all: a headless render never acquires or presents the swapchain (it
    submits, then reads back offscreen), so a hook living only beside
    vkQueuePresentKHR is unreachable from every test in the repo - which is
    exactly how the first version of this hook shipped, firing zero times.
    """
    codes = "VK_ERROR_DEVICE_LOST,VK_ERROR_SURFACE_LOST_KHR,VK_ERROR_DEVICE_LOST"
    probe_env = dict(env, VF_TRACE="1", VF_TEST_FORCE_PRESENT_ERR=codes)
    run = subprocess.run(
        [binary, "--smoke", "30", "--width", "64", "--height", "36"],
        capture_output=True, text=True, timeout=900, env=probe_env,
    )
    log = (run.stdout or "") + "\n" + (run.stderr or "")
    if run.returncode != 0:
        failures.append("present probe: smoke run failed")
        return
    expectations = [
        ("VK_ERROR_DEVICE_LOST", "error", 1),
        ("VK_ERROR_SURFACE_LOST_KHR", "warning", 1),
    ]
    for name, level, want in expectations:
        # the level tag is part of the same line, so a wrong severity fails
        found = len(re.findall(r"\[%s\].*present returned %s" % (level, name), log))
        any_level = len(re.findall(r"present returned %s" % name, log))
        if any_level == 0:
            failures.append(f"present probe: {name} was never reported - the "
                            "probe did not fire")
        elif found != want:
            sev = "wrong severity" if any_level == want else \
                  f"reported {any_level}x, expected {want} (log-once broken)"
            failures.append(f"present probe: {name} {sev}")
        else:
            print(f"[present probe] {name}: 1 line at {level}")


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    binary = os.path.abspath(sys.argv[1])
    src_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    manifest_path = os.path.join(
        os.environ.get("VOXELFORGE_ASSET_DIR", os.path.join(src_root, "assets")),
        "world.json",
    )
    manifest_before = None
    if os.path.exists(manifest_path):
        with open(manifest_path, "rb") as manifest_file:
            manifest_before = manifest_file.read()
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        # One process renders every view (--shotlist): a world load is ~13 s
        # and dominates a single --shot, so batching is what makes the suite
        # affordable.
        list_path = os.path.join(tmp, "shots.txt")
        outs = []
        with open(list_path, "w") as lf:
            shots = SHOTS[:1] if FAST else SHOTS
            for name, cam in shots:
                out = os.path.join(tmp, name + ".ppm")
                outs.append((name, out))
                lf.write(" ".join([out, *cam]) + "\n")
        env = dict(os.environ, VF_NO_OVERLAY="1")
        cmd = [binary, "--shotlist", list_path,
               "--width", str(W), "--height", str(H)]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=900, env=env)
        if r.returncode != 0:
            print(r.stderr[-2000:])
            print("FAIL: batch render failed")
            return 1
        for name, out in outs:
            if not os.path.exists(out):
                failures.append(f"{name}: render produced no output")
                continue
            m = analyze(out)
            print(
                f"[{name}] coverage {m['obj_frac']*100:.1f}%  "
                f"black-in-silhouette {m['black_in_obj']*100:.2f}%  "
                f"sky-probe {'ok' if m['sky_probe_ok'] else 'BAD'}"
            )
            # Coverage bounds: a healthy frame shows mostly world with some sky.
            # The upper bound is 98.5% (not 97%): the water close-up frames a
            # clear shallow cove where water/bed/pebbles/rapids legitimately
            # fill the frame (they count as non-sky pixels). A buried camera
            # still fails via ~100% coverage together with black-in-silhouette.
            if not (0.03 <= m["obj_frac"] <= 0.985):
                failures.append(f"{name}: coverage {m['obj_frac']:.3f} out of range")
            if m["black_in_obj"] > 0.05:
                failures.append(
                    f"{name}: black-in-silhouette {m['black_in_obj']*100:.2f}% > 5%"
                )
            if not m["sky_probe_ok"]:
                failures.append(f"{name}: sky probe not blue-dominant")

        # Ownership regression: a selected .vxw layer must be exactly one
        # stable owner class, not the union of overlapping scene chunks. The
        # before/after masks also prove that the live transform moves the
        # selected green layer while leaving other red object layers fixed.
        # Both synthetic rotate hooks only arm the preview; neither may commit.
        ownership_masks = {}
        subject = ownership_layer(manifest_path, failures)
        for label, angles in (("before", "0,0,0"), ("after", "45,15,-10")):
            if subject is None:
                break
            ownership_out = os.path.join(tmp, f"ownership-{label}.ppm")
            ownership_env = dict(
                env,
                VF_SPLAT_DEBUG="16",
                VF_TEST_ROTATE_LIVE=angles,
                VF_TEST_TRACKBALL_PROBE="1",
                VF_ROTATE_LAYER=subject,
            )
            ownership_cmd = [
                binary,
                "--shot",
                ownership_out,
                "--cam",
                *SHOTS[0][1],
                "--width",
                str(W),
                "--height",
                str(H),
            ]
            ownership_run = subprocess.run(
                ownership_cmd,
                capture_output=True,
                text=True,
                timeout=900,
                env=ownership_env,
            )
            if ownership_run.returncode != 0:
                print(ownership_run.stderr[-2000:])
                failures.append(f"layer ownership {label}: debug render failed")
            elif not os.path.exists(ownership_out):
                failures.append(f"layer ownership {label}: render produced no output")
            else:
                ownership_log = (ownership_run.stdout or "") + "\n" + (ownership_run.stderr or "")
                probe = next(
                    (line for line in ownership_log.splitlines()
                     if "trackball probe:" in line),
                    None,
                )
                if not probe:
                    failures.append(
                        f"trackball {label}: projection/ring probe was not emitted"
                    )
                else:
                    match = re.search(
                        r"centre=\(([-+0-9.]+),([-+0-9.]+)\) "
                        r"radius=([-+0-9.]+) hitYaw=([0-9]+) "
                        r"hitPitch=([0-9]+) hitRoll=([0-9]+) "
                        r"hitCentre=([0-9]+) "
                        r"bounds=\(([-+0-9.]+),([-+0-9.]+),([-+0-9.]+)\)-"
                        r"\(([-+0-9.]+),([-+0-9.]+),([-+0-9.]+)\) "
                        r"camPos=\(([-+0-9.]+),([-+0-9.]+),([-+0-9.]+)\) "
                        r"camYaw=([-+0-9.]+) camPitch=([-+0-9.]+)",
                        probe,
                    )
                    if not match:
                        failures.append(
                            f"trackball {label}: malformed projection probe: {probe}"
                        )
                    else:
                        expected = expected_trackball(match, W, H)
                        geometry_ok = expected is not None and (
                            0.0 <= float(match.group(1)) <= W
                            and 0.0 <= float(match.group(2)) <= H
                            and float(match.group(3)) > 0.0
                            and abs(float(match.group(1)) - expected[0]) < 0.75
                            and abs(float(match.group(2)) - expected[1]) < 0.75
                            and abs(float(match.group(3)) - expected[2]) < 0.75
                        )
                        if not geometry_ok or match.group(4, 5, 6, 7) != (
                            "1", "2", "3", "0"
                        ):
                            failures.append(
                                f"trackball {label}: wrong projected centre/ring hit: {probe}"
                            )
                ownership_masks[label] = ownership_classes(ownership_out)
                ownership = analyze_ownership(ownership_out)
                print(
                    f"[ownership {label}] selected {ownership['selected']*100:.2f}%  "
                    f"other-object {ownership['other_object']*100:.2f}%  "
                    f"terrain-water {ownership['terrain_water']*100:.2f}%"
                )
                if ownership["selected"] < 0.003:
                    failures.append(
                        f"layer ownership {label}: selected layer is not green"
                    )
                if ownership["other_object"] < 0.01:
                    failures.append(
                        f"layer ownership {label}: other object layers are not red"
                    )
                if ownership["terrain_water"] < 0.20:
                    failures.append(
                        f"layer ownership {label}: terrain/water is not blue"
                    )
        if "before" in ownership_masks and "after" in ownership_masks:
            w, h, before = ownership_masks["before"]
            ow, oh, after = ownership_masks["after"]
            if (w, h) != (ow, oh):
                failures.append("layer ownership: mask dimensions differ")
            else:
                n = w * h
                selected_change = sum(
                    (a == 1) != (b == 1) for a, b in zip(before, after)
                ) / n
                other_change = sum(
                    (a == 2) != (b == 2) for a, b in zip(before, after)
                ) / n
                terrain_change = sum(
                    (a == 3) != (b == 3) for a, b in zip(before, after)
                ) / n
                print(
                    f"[ownership motion] selected {selected_change*100:.2f}%  "
                    f"other-object {other_change*100:.2f}%  "
                    f"terrain-water {terrain_change*100:.2f}%"
                )
                if selected_change < 0.001:
                    failures.append("layer ownership: selected layer did not move")
                if other_change > 0.005:
                    failures.append("layer ownership: another object layer moved")
                if terrain_change > 0.01:
                    failures.append("layer ownership: terrain/water mask moved")
        if FAST:
            # Move handles are screen-space lines, not world-space lengths:
            # probe each colored X/Y/Z tip independently so a missing or
            # overlapping Y/Z handle cannot regress silently. The layer is the
            # manifest-resolved subject (it used to be a second hard-coded
            # "hamlet_cabin.vxw", which went stale the same way as the rotate
            # one and made this probe silently not emit).
            move_out = os.path.join(tmp, "move_probe.ppm")
            move_env = dict(
                env,
                VF_TEST_MOVE_LIVE=f"1,0,0,{subject or ''}",
                VF_TEST_MOVE_PROBE="1",
            )
            move_run = subprocess.run(
                [binary, "--shot", move_out, "--cam", *SHOTS[0][1],
                 "--width", str(W), "--height", str(H)],
                capture_output=True, text=True, timeout=900, env=move_env,
            )
            if move_run.returncode != 0:
                failures.append("move probe: render failed")
            else:
                move_log = (move_run.stdout or "") + "\n" + (move_run.stderr or "")
                move_match = re.search(
                    r"move probe: hitX=([0-9]+) hitY=([0-9]+) hitZ=([0-9]+)",
                    move_log,
                )
                if not move_match:
                    failures.append("move probe: axis handle probe was not emitted")
                elif move_match.groups() != ("1", "2", "3"):
                    failures.append(
                        f"move probe: expected X/Y/Z hits 1/2/3, got "
                        f"{'/'.join(move_match.groups())}"
                    )
                else:
                    print("[move probe] X/Y/Z handles independently selectable")
        # Canopy parity: the splat backend must not paint the tree black where
        # the SVO reference lights it. Two extra renders (one per backend) at
        # the user's own camera; the env above sets VF_NO_OVERLAY, so a
        # session's live edits cannot pollute the reference frame.
        check_canopy_parity(binary, tmp, env, failures)
        if not FAST:
            check_present_probe(binary, tmp, env, failures)
        if manifest_before is not None:
            with open(manifest_path, "rb") as manifest_file:
                manifest_after = manifest_file.read()
            if manifest_after != manifest_before:
                # A concurrent WRITER can trip this guard, not only the code
                # under test. Another session's A/B harness legitimately
                # toggles a layer's `enabled` flag in this same file for the
                # duration of a render (~1 s), and even an atomic temp+rename
                # write is still observable between its disable and restore
                # (there is no env override for the compile-time
                # VOXELFORGE_ASSET_DIR). One settled re-read separates that
                # transient from a REAL write by the app under test: the
                # preview hooks (VF_TEST_ROTATE_LIVE / VF_TEST_MOVE_LIVE) never
                # commit, so a byte difference that SURVIVES a 2 s settle is a
                # genuine commit and still fails below.
                time.sleep(2.0)
                with open(manifest_path, "rb") as manifest_file:
                    manifest_after = manifest_file.read()
            if manifest_after != manifest_before:
                failures.append("layer ownership: preview hook modified world.json")
            else:
                print("[ownership manifest] unchanged")
    if failures:
        for f in failures:
            print("FAIL:", f)
        return 1
    print("visual_check FAST PASSED" if FAST else "visual_check PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
