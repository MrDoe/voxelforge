#!/usr/bin/env python3
"""Gate for the MICRO-VOXEL overlay (separate SVO worlds, own fine lattice).

The micro subsystem had NO automated coverage at all, and that is how a whole
class of bugs shipped: the vase rendered as a dark, almost black object
because the SDF's byte encoding was a whole-voxel truncation (fine for the
world's DDA, fatal for a sphere tracer), and the trace converged onto the
world's bounding box instead of the object.

This gate is the measurement that caught it, kept as a script so the claims
are reproducible rather than remembered:

  1. MATERIAL - the micro objects must be visible AND bright and neutral.
     The bug's signature was a near-black, slightly green object: the air
     filler's black albedo with the grass material. A mean luma well below the
     plaster's, or a channel spread, fails.
  2. SHAPE - the silhouette must not be a rectangle. The "hits the bounding
     box" failure mode produced a 67x67 px SQUARE, and a mean colour cannot
     see that; a fill-ratio test can.
  3. MICRO-TO-MICRO SHADOW - with the vase present the urn must be DARKER on
     the pixels it occupies. One object cannot show this, which is why two are
     shipped.
  4. TWO-WAY SHADOW - the micro objects must RECEIVE the main world's coarse
     shadow (their luma with the world shadow term off must be brighter), and
     they must CAST onto the world (the world bake reports surfels darkened
     by their fields). Both halves are coarse by design: the receive half is
     the heightfield + coarse object volume, the cast half is one binary
     verdict per baked surfel.
  5. NO WORLD REGRESSION - the house view must be pixel-identical with the
     micro layers on and off. The overlay must not be able to touch the 0.1 m
     path.
  6. SELECTABLE - a micro object lives outside the world's store, so the
     pointer needs a pick of its own. The app's own pick path is exercised
     headlessly (VF_TEST_MICRO_PICK), because a pick that silently misses is
     invisible in a screenshot: the object renders perfectly and cannot be
     clicked.

SCOPE: this gate covers the VISIBLE outcome. It does NOT cover the SDF's
quantisation - measured: with the whole-voxel encoding restored, this gate
still passes (the dense field's search falls back to a deeper solid cell, and
the vase is uniformly coloured so the material still reads as plaster). The
quantisation is pinned by tests/test_micro.cpp instead ("the byte quantisation
keeps the half-voxel offsets"), which fails loudly on it. The two layers are
complementary: the unit test pins the encoding, this gate pins what the
encoding is FOR.

Usage:  python3 tests/micro_check.py [path/to/voxelforge]
"""
from __future__ import annotations

import json
import os
import re
import statistics
import subprocess
import sys
import tempfile

# The repo root, so the script works from the build directory too (ctest runs
# there) and not only from the source root.
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "voxelforge")

# The micro objects' spot on the shore: the vase at (12, -0.25, 12) and the urn
# at (12.3, -0.25, 12.2), the latter in the vase's shadow (the sun is at
# 34 deg / 238 deg, so shadows are thrown toward (0.85, 0.53) in xz).
MICRO_CAM = "11.45,0.30,11.45,12.22,-0.20,12.14"
# A camera that fills the frame with the ALPACA (the third micro object).
ALPACA_CAM = "10.95,0.85,11.45,11.75,0.52,12.55"
# A view with no micro object in it: the house.
HOUSE_CAM = "2.5,1.3,6.0,6.8,1.0,12.2"
W, H = 900, 600

failures: list[str] = []


def note(ok: bool, msg: str) -> None:
    print(("  ok   " if ok else "  FAIL ") + msg)
    if not ok:
        failures.append(msg)


def render(out: str, cam: str, w: int = W, h: int = H, mode: str = "",
           flags: str = "") -> None:
    cmd = [APP, "--shot", out, "--cam", cam, "--width", str(w), "--height", str(h)]
    if mode:
        cmd += ["--mode", mode]
    env = os.environ.copy()
    if flags:
        env["VF_RENDER_FLAGS"] = flags
    subprocess.run(
        cmd,
        check=True,
        cwd=ROOT,
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def read_ppm(path: str) -> tuple[int, int, bytes]:
    with open(path, "rb") as f:
        f.readline()  # P6
        w, h = map(int, f.readline().split())
        f.readline()  # maxval
        return w, h, f.read()


MANIFEST = os.path.join(ROOT, "assets", "world.json")


def _write_manifest(doc: dict) -> None:
    """The manifest is a TRACKED file, so it is rewritten through a temp file
    and renamed: a bare write truncates it, and a concurrent reader (another
    session's render, a running app's poll) would see a half-written manifest.
    """
    tmp = MANIFEST + ".microcheck.tmp"
    with open(tmp, "w") as f:
        json.dump(doc, f, indent=2)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, MANIFEST)


def set_micro(enabled: dict[str, bool], rot: dict[str, float] | None = None) -> None:
    """Toggle the micro layers in the manifest (and optionally set a rotation)."""
    with open(MANIFEST) as f:
        doc = json.load(f)
    for layer in doc["layers"]:
        if layer.get("role") == "micro":
            name = layer.get("file", "")
            layer["enabled"] = bool(enabled.get(name, True))
            if rot is not None and name in rot:
                layer["rot"] = rot[name]
    _write_manifest(doc)


# The manifest's micro entries as they were found, so the gate can put them
# back EXACTLY. A restore that only re-enables the layers leaves a rotation
# behind (measured: the gate turned the shipped alpaca 90 degrees and left it
# that way).
_MICRO_SAVED: list[tuple[str, bool, float]] = []


def save_micro() -> None:
    """Snapshot the micro entries' VALUES (not their dicts: the manifest is
    re-read on every write, so a saved dict reference would be stale)."""
    with open(MANIFEST) as f:
        doc = json.load(f)
    _MICRO_SAVED.clear()
    for layer in doc["layers"]:
        if layer.get("role") == "micro":
            _MICRO_SAVED.append(
                (layer.get("file", ""), layer.get("enabled", True),
                 float(layer.get("rot", 0.0)))
            )


def restore_micro() -> None:
    """Put the manifest back exactly as it was found."""
    if not _MICRO_SAVED:
        return
    with open(MANIFEST) as f:
        doc = json.load(f)
    saved = {name: (enabled, rot) for name, enabled, rot in _MICRO_SAVED}
    for layer in doc["layers"]:
        if layer.get("role") != "micro":
            continue
        entry = saved.get(layer.get("file", ""))
        if entry:
            layer["enabled"] = entry[0]
            layer["rot"] = entry[1]
        # A key the gate may have introduced (an older run wrote "rotDeg")
        # would otherwise survive into the shipped manifest.
        layer.pop("rotDeg", None)
    _write_manifest(doc)


def differing(a: bytes, b: bytes, thr: int = 16) -> list[int]:
    return [
        i
        for i in range(0, len(a), 3)
        if max(abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]), abs(a[i + 2] - b[i + 2])) > thr
    ]


def luma(buf: bytes, i: int) -> int:
    return (buf[i] * 299 + buf[i + 1] * 587 + buf[i + 2] * 114) // 1000


def main() -> int:
    tmpdir = tempfile.mkdtemp(prefix="vf-micro-check-")
    env = os.environ.copy()
    # Never touch the live-edit overlay file: this is a read-only measurement.
    env["VF_OVERLAY_PATH"] = os.path.join(tmpdir, "overlay.vxw")
    os.environ.update(env)

    try:
        save_micro()
        on = os.path.join(tmpdir, "on.ppm")
        off = os.path.join(tmpdir, "off.ppm")
        urn_only = os.path.join(tmpdir, "urn.ppm")

        print("[micro] rendering the micro objects ...")
        set_micro({"micro/vase.vxw": True, "micro/urn.vxw": True})
        render(on, MICRO_CAM)
        set_micro({"micro/vase.vxw": False, "micro/urn.vxw": False})
        render(off, MICRO_CAM)
        set_micro({"micro/vase.vxw": False, "micro/urn.vxw": True})
        render(urn_only, MICRO_CAM)

        _, _, a = read_ppm(on)
        _, _, b = read_ppm(off)
        _, _, c = read_ppm(urn_only)

        px = differing(a, b)
        note(len(px) > 500, "micro objects are visible (%d differing pixels)" % len(px))
        if not px:
            print("FAIL: no micro pixels - nothing to measure")
            return 1

        # 1. MATERIAL: not the black air filler, and neutral.
        #    The bug's signature was a near-black, slightly GREEN object (the
        #    air filler's black albedo with the grass material) - measured
        #    (17, 69, 23). Two checks catch it, and they are deliberately
        #    relative rather than absolute: the objects sit in a real shadow,
        #    so their absolute luma depends on the scene, not on the material.
        #    (An absolute "bright" threshold has to be loose enough for a
        #    shadowed object and tight enough to reject the bug - and the
        #    NEUTRAL check is the one that really separates them.)
        mean = [statistics.mean(a[i + k] for i in px) for k in range(3)]
        note(
            min(mean) > 40.0,
            "material is not the black air filler (mean RGB %.0f/%.0f/%.0f, want > 40)"
            % (mean[0], mean[1], mean[2]),
        )
        note(
            max(mean) - min(mean) < 25.0,
            "material is neutral (channel spread %.1f, want < 25)"
            % (max(mean) - min(mean)),
        )

        # 2. SHAPE: not a rectangle. A rectangle's bounding box is filled; a
        #    vase's is not. The "hits the bounding box" failure produced a
        #    67x67 px square, which a mean colour cannot see.
        xs = [(i // 3) % W for i in px]
        ys = [(i // 3) // W for i in px]
        bw = max(xs) - min(xs) + 1
        bh = max(ys) - min(ys) + 1
        fill = len(px) / float(bw * bh)
        note(
            fill < 0.85,
            "silhouette is not a filled rectangle (bbox %dx%d, fill %.2f, want < 0.85)"
            % (bw, bh, fill),
        )

        # 3. MICRO-TO-MICRO SHADOW: the urn's own pixels must be darker with
        #    the vase present.
        urn_px = differing(c, b)
        if not urn_px:
            note(False, "the urn alone is not visible")
        else:
            darker = [i for i in urn_px if luma(a, i) < luma(c, i) - 3]
            mean_drop = (
                statistics.mean(luma(c, i) for i in darker)
                - statistics.mean(luma(a, i) for i in darker)
                if darker
                else 0.0
            )
            note(
                len(darker) > 30 and mean_drop > 8.0,
                "the vase shadows the urn (%d px darkened, mean drop %.1f luma)"
                % (len(darker), mean_drop),
            )

        # 4. TWO-WAY SHADOW.
        #    (a) RECEIVE: with the world's shadow term disabled the micro
        #        objects must be BRIGHTER. The shipped spot is in the shadow
        #        of a real world object, so this is a real verdict, not a
        #        tautology.
        print("[micro] checking the two-way shadow ...")
        no_shadow = os.path.join(tmpdir, "on_noshadow.ppm")
        set_micro({"micro/vase.vxw": True, "micro/urn.vxw": True})
        render(no_shadow, MICRO_CAM, flags="253")
        _, _, ns = read_ppm(no_shadow)
        lit = statistics.mean(luma(ns, i) for i in px)
        shaded = statistics.mean(luma(a, i) for i in px)
        note(
            shaded < lit * 0.97,
            "micro objects RECEIVE the world's shadow (%.0f lit vs %.0f shadowed luma)"
            % (lit, shaded),
        )
        #    (b) CAST: the world bake must report surfels darkened by the
        #        micro fields. A "cast shadow" that darkens nothing is the
        #        failure mode to watch for, and it is silent.
        proc = subprocess.run(
            [APP, "--shot", os.path.join(tmpdir, "bake.ppm"), "--cam", MICRO_CAM,
             "--width", "320", "--height", "200"],
            cwd=ROOT, env=os.environ.copy(), capture_output=True, text=True,
            check=True,
        )
        m = re.search(r"micro cast shadow darkened (\d+) surfel", proc.stdout + proc.stderr)
        note(
            m is not None and int(m.group(1)) >= 5,
            "micro objects CAST onto the world (%s surfels darkened)"
            % (m.group(1) if m else "no report at all"),
        )

        # 4c. FUR. The alpaca's fleece is a TEXTURED material (21), and a
        #     textured material ignores the record's colour - so a binding
        #     that silently failed would fall back to a FLAT cream and every
        #     other check here would still pass. The fur's signature is local
        #     contrast: a flat palette fill has almost none.
        print("[micro] checking the alpaca's fleece ...")
        fur_on = os.path.join(tmpdir, "fur_on.ppm")
        fur_off = os.path.join(tmpdir, "fur_off.ppm")
        set_micro({"micro/vase.vxw": True, "micro/urn.vxw": True,
                   "micro/alpaca.vxw": True})
        render(fur_on, ALPACA_CAM, 700, 700)
        set_micro({"micro/vase.vxw": False, "micro/urn.vxw": False,
                   "micro/alpaca.vxw": False})
        render(fur_off, ALPACA_CAM, 700, 700)
        _, _, fa = read_ppm(fur_on)
        _, _, fb = read_ppm(fur_off)
        apx = differing(fa, fb, 14)
        note(len(apx) > 2000, "the alpaca is visible (%d px)" % len(apx))
        if apx:
            lum = [luma(fa, i) for i in apx]
            mean = statistics.mean(lum)
            # mean absolute deviation from a 3x3-ish blur is overkill; the
            # spread of the luma itself is enough to tell fur from flat paint.
            sd = statistics.pstdev(lum)
            note(
                sd > 8.0 and mean > 40.0,
                "the fleece carries texture (luma mean %.0f, spread %.1f)"
                % (mean, sd),
            )

        # 4d. LIVE MOVEMENT: a micro layer's `rot` must actually turn the
        #     object, and the object must stay PUT (the rotation is about the
        #     bottom-centre pivot, so the shape spins without drifting).
        #     Measured by the footprint from directly above: a 90 degree yaw
        #     swaps the alpaca's long axis, so the footprint's aspect flips.
        print("[micro] checking the live movement (rotation) ...")
        # TWO views, because they measure different things:
        #  * from directly above, the screen's x/y ARE the world x/z, so the
        #    footprint's aspect is camera-independent - that is the "did it
        #    turn" measure (the alpaca is long and thin, so a yaw flips it);
        #  * from the side, the screen's y is the world height, so the
        #    silhouette's base row IS the pivot's projection - that is the
        #    "did it stay put" measure. From directly above the base row would
        #    be a world x/z and would measure nothing.
        top_cam = "11.72,1.85,12.32,11.78,-0.25,12.30"
        side_cam = ALPACA_CAM

        def footprint(rot_deg: float, cam: str) -> tuple[int, int, int, int, int] | None:
            tag = "%d_%s" % (rot_deg, "top" if cam is top_cam else "side")
            on = os.path.join(tmpdir, "rot%s_on.ppm" % tag)
            off = os.path.join(tmpdir, "rot%s_off.ppm" % tag)
            set_micro({"micro/vase.vxw": False, "micro/urn.vxw": False,
                       "micro/alpaca.vxw": False})
            render(off, cam, 560, 560)
            set_micro({"micro/vase.vxw": True, "micro/urn.vxw": True,
                       "micro/alpaca.vxw": True},
                      {"micro/alpaca.vxw": rot_deg})
            render(on, cam, 560, 560)
            _, _, oa = read_ppm(on)
            _, _, ob = read_ppm(off)
            pxs = differing(oa, ob, 14)
            if not pxs:
                return None
            xs = [(i // 3) % 560 for i in pxs]
            ys = [(i // 3) // 560 for i in pxs]
            return (min(xs), min(ys), max(xs), max(ys), len(pxs))

        t0 = footprint(0.0, top_cam)
        t90 = footprint(90.0, top_cam)
        s0 = footprint(0.0, side_cam)
        s90 = footprint(90.0, side_cam)
        note(
            all(x is not None for x in (t0, t90, s0, s90)),
            "the alpaca is measurable at both angles",
        )
        if t0 and t90 and s0 and s90:
            # DID IT TURN: from above, the footprint is the object's x/z
            # extent, and a 90 degree yaw swaps the long axis, so the aspect
            # flips.
            a0 = (t0[2] - t0[0]) / max(1, t0[3] - t0[1])
            a90 = (t90[2] - t90[0]) / max(1, t90[3] - t90[1])
            note(
                a0 > 1.0 > a90,
                "a 90 degree yaw turns the object (footprint aspect %.2f -> %.2f)"
                % (a0, a90),
            )
            # DID IT STAY PUT: from the side, the silhouette's base row is the
            # pivot's projection and must not move.
            note(
                abs(s0[3] - s90[3]) < 12,
                "the object stays put while it turns (base row %d -> %d)"
                % (s0[3], s90[3]),
            )

        # 5. SELECTABLE. A micro object is NOT in the world's store - it is a
        #    separate SVO world on its own fine lattice - so the pointer needs
        #    a pick of its own, and that pick is invisible in a screenshot.
        #    The bug this pins: the field's CPU sampler returned 0 outside
        #    the field's grid, so the pick's sphere-trace stepped 0 and
        #    crawled at its minimum step - it covered ~0.6 m in its whole
        #    budget and never reached an object 15 m away. The alpaca
        #    rendered perfectly and could not be clicked.
        #    The app's OWN pick path (pickMicroObjects) is exercised
        #    headlessly by VF_TEST_MICRO_PICK - no mouse, no synthetic
        #    input, the same call the pointer makes.
        print("[micro] checking that the micro objects are selectable ...")
        set_micro({"micro/vase.vxw": True, "micro/urn.vxw": True,
                   "micro/alpaca.vxw": True})
        pick_env = os.environ.copy()
        pick_env["VF_TEST_MICRO_PICK"] = "1"
        pick_env["VF_OVERLAY_PATH"] = os.path.join(tmpdir, "overlay.vxw")
        proc = subprocess.run(
            [APP, "--shot", os.path.join(tmpdir, "pick.ppm"), "--cam", MICRO_CAM,
             "--width", "320", "--height", "200"],
            cwd=ROOT, env=pick_env, capture_output=True, text=True, check=True,
        )
        m = re.search(r"VF_TEST_MICRO_PICK: (\d+)/(\d+) micro objects selectable",
                      proc.stdout + proc.stderr)
        note(
            m is not None and int(m.group(2)) >= 1
            and int(m.group(1)) == int(m.group(2)),
            "the micro objects are selectable (%s)"
            % (m.group(0) if m else "no report at all"),
        )

        # 6. NO WORLD REGRESSION: the house view is untouched.
        print("[micro] rendering the house view (no micro object in it) ...")
        house_on = os.path.join(tmpdir, "house_on.ppm")
        house_off = os.path.join(tmpdir, "house_off.ppm")
        set_micro({"micro/vase.vxw": True, "micro/urn.vxw": True})
        render(house_on, HOUSE_CAM, 960, 540)
        set_micro({"micro/vase.vxw": False, "micro/urn.vxw": False})
        render(house_off, HOUSE_CAM, 960, 540)
        _, _, ha = read_ppm(house_on)
        _, _, hb = read_ppm(house_off)
        hpx = differing(ha, hb)
        note(
            len(hpx) == 0,
            "the world view is untouched by the overlay (%d differing pixels)" % len(hpx),
        )

        # 5. BOTH BACKENDS: the micro objects must draw in the SVO reference
        #    backend too.
        print("[micro] rendering the SVO backend ...")
        svo_on = os.path.join(tmpdir, "svo_on.ppm")
        svo_off = os.path.join(tmpdir, "svo_off.ppm")
        set_micro({"micro/vase.vxw": True, "micro/urn.vxw": True})
        render(svo_on, MICRO_CAM, mode="svo")
        set_micro({"micro/vase.vxw": False, "micro/urn.vxw": False})
        render(svo_off, MICRO_CAM, mode="svo")
        _, _, sa = read_ppm(svo_on)
        _, _, sb = read_ppm(svo_off)
        spx = differing(sa, sb)
        note(len(spx) > 200, "the SVO backend draws the micro objects (%d px)" % len(spx))
    finally:
        restore_micro()

    print()
    if failures:
        print("FAIL: %d micro check(s) failed" % len(failures))
        for f in failures:
            print("  - " + f)
        return 1
    print("PASS: micro-voxel overlay")
    return 0


if __name__ == "__main__":
    sys.exit(main())
