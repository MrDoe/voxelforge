#!/usr/bin/env python3
"""Gate for the FUR-CARD layer over the micro-voxel objects.

The fur is the one part of the renderer that is TRANSLUCENT on purpose: a
coat of hair cards, each a flat ribbon standing out of the surface, composited
over the already-opaque body. The world's own splats cannot do that - their
opaque base seal (the nearest fragment written at alpha 1) is exactly what
turns a lone silhouette splat into a hard-edged blob - so the fur is a
separate pass and a separate layer.

What this gate measures, and why each one is a measurement rather than a
screenshot:

  1. THE FUR EXISTS. The alpaca's pixels must change when the fur is on.
     The instrument's NOISE FLOOR is measured first, from two renders of the
     SAME configuration (the scene's animated grass and the float16
     G-buffer move a little between runs), and the signal has to clear it.
  2. THE FUR IS CONFINED. The changed pixels must be a COMPACT BLOB (a high
     fill ratio inside their own bounding box) and must not spread over the
     frame. A DDA that walked off the object would light up the sky, and a
     coverage that leaked would wash the whole scene: both are invisible in
     a caption and obvious in a count.
  3. THE FUR TOUCHES NOTHING ELSE. The house view (no micro object in it) and
     the vase/urn view (micro objects with no fur material) must be
     UNCHANGED at the noise floor. The vase and the urn ride the same
     micro path as the alpaca, so this also proves the fur is a MATERIAL
     decision and not a "everything micro is furry" one.
  4. THE FUR IS BACKEND-INDEPENDENT. It is its own pass, so it must appear
     in the SVO reference backend too. A fur that only existed in the splat
     backend would be a splat-backend feature wearing a pass's clothes.
  5. THE ESCAPE HATCH. With VF_FUR=0 the cards are never generated, the pass
     is empty and its dispatch is skipped, so the frame must be identical to
     the fur-on frame OUTSIDE the alpaca - and the log must say that no fur
     object was loaded at all.

Usage:  python3 tests/fur_check.py [path/to/voxelforge]
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "voxelforge")

# The camera that fills the frame with the ALPACA is DERIVED from the
# manifest's own placement, because the animal is placed by the scene (and it
# has been moved before). The two ceramic objects (which carry no fur) and one
# view with no micro object in it at all are fixed.
# A camera BETWEEN the ceramic objects and the alpaca, looking back at the
# vase: the two objects that carry NO fur fill the frame and the fur-bearing
# animal is behind the camera, so the frame measures the fur's effect on
# objects that must not have one.
MICRO_CAM = "12.15,0.35,13.60,12.15,-0.05,12.20"
HOUSE_CAM = "2.5,1.3,6.0,6.8,1.0,12.2"
W, H = 900, 600
MANIFEST = os.path.join(ROOT, "assets", "world.json")

# The noise floor of a same-configuration pair. Measured, not assumed: the
# scene's animated grass and the float16 G-buffer move a little between runs.
NOISE_MAX = 6

failures: list[str] = []


def note(ok: bool, msg: str) -> None:
    print(("  ok   " if ok else "  FAIL ") + msg)
    if not ok:
        failures.append(msg)


def render(out: str, cam: str, mode: str = "", env_extra: dict | None = None) -> str:
    cmd = [APP, "--shot", out, "--cam", cam, "--width", str(W), "--height", str(H)]
    if mode:
        cmd += ["--mode", mode]
    env = os.environ.copy()
    # Never touch the live-edit overlay file: this is a read-only measurement.
    env["VF_OVERLAY_PATH"] = os.path.join(OUTDIR, "overlay.vxw")
    if env_extra:
        env.update(env_extra)
    proc = subprocess.run(cmd, check=True, cwd=ROOT, env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return proc.stdout.decode("utf-8", "replace")


def read_ppm(path: str) -> tuple[int, int, bytes]:
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        w, h = map(int, f.readline().split())
        f.readline()
        return w, h, f.read()


def diff_stats(a: bytes, b: bytes, thr: int) -> tuple[int, int, int, int, int]:
    """The changed pixels above `thr`, and their bounding box."""
    n = len(a) // 3
    cnt = 0
    minx, miny, maxx, maxy = 10**9, 10**9, -1, -1
    for i in range(n):
        o = i * 3
        d = max(abs(a[o] - b[o]), abs(a[o + 1] - b[o + 1]), abs(a[o + 2] - b[o + 2]))
        if d > thr:
            cnt += 1
            x, y = i % W, i // W
            minx, maxx = min(minx, x), max(maxx, x)
            miny, maxy = min(miny, y), max(maxy, y)
    if cnt == 0:
        return 0, 0, 0, 0, 0
    box = (maxx - minx + 1) * (maxy - miny + 1)
    return cnt, minx, miny, maxx, maxy, box  # type: ignore[return-value]


# ---- the manifest: the alpaca must STAND STILL for a measurement ----------
#
# The alpaca is a GRAZER: it walks around its anchor, so two renders of the
# same configuration differ by tens of thousands of pixels and every
# comparison in this gate would measure the GAIT instead of the fur. The
# gate therefore clears the behaviour for the duration and restores it
# afterwards - and the restore is VERIFIED, because the app rewrites
# world.json from its own layer list, so a write can land after the gate's
# restore and silently leave the scene changed.
_MICRO_SAVED: list[tuple[str, bool, float, str]] = []


def _read_manifest() -> dict:
    with open(MANIFEST) as f:
        return json.load(f)


def _write_manifest(doc: dict) -> None:
    """The manifest is a TRACKED file, so it is rewritten through a temp file
    and renamed: a bare write truncates it, and a concurrent reader (another
    session's render, a running app's poll) would see a half-written file.
    """
    tmp = MANIFEST + ".furcheck.tmp"
    with open(tmp, "w") as f:
        json.dump(doc, f, indent=2)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, MANIFEST)


def save_micro() -> None:
    """Snapshot the micro layers' VALUES (not their dicts: the manifest is
    re-read on every write, so a saved dict reference would be stale)."""
    doc = _read_manifest()
    _MICRO_SAVED.clear()
    for layer in doc["layers"]:
        if layer.get("role") == "micro":
            _MICRO_SAVED.append(
                (layer.get("file", ""), bool(layer.get("enabled", True)),
                 float(layer.get("rot", 0.0)), str(layer.get("behaviour", "")))
            )


def set_behaviour(file: str, behaviour: str) -> None:
    doc = _read_manifest()
    for layer in doc["layers"]:
        if layer.get("file", "") == file:
            if behaviour:
                layer["behaviour"] = behaviour
            else:
                layer.pop("behaviour", None)
    _write_manifest(doc)


def restore_micro() -> bool:
    """Put the manifest back exactly as it was found, and VERIFY it: the app
    rewrites world.json from its own layer list, so its write can beat the
    gate's restore. A restore that did not take is reported, not assumed."""
    if not _MICRO_SAVED:
        return True
    for attempt in range(3):
        doc = _read_manifest()
        saved = {name: (enabled, rot, beh)
                 for name, enabled, rot, beh in _MICRO_SAVED}
        ok = True
        for layer in doc["layers"]:
            if layer.get("role") != "micro":
                continue
            entry = saved.get(layer.get("file", ""))
            if not entry:
                continue
            enabled, rot, beh = entry
            layer["enabled"] = enabled
            layer["rot"] = rot
            if beh:
                layer["behaviour"] = beh
            else:
                layer.pop("behaviour", None)
        _write_manifest(doc)
        # verify
        doc2 = _read_manifest()
        for layer in doc2["layers"]:
            if layer.get("role") != "micro":
                continue
            entry = saved.get(layer.get("file", ""))
            if not entry:
                continue
            enabled, rot, beh = entry
            if (bool(layer.get("enabled", True)) != enabled
                    or float(layer.get("rot", 0.0)) != rot
                    or str(layer.get("behaviour", "")) != beh):
                ok = False
        if ok:
            return True
        time.sleep(0.5)
    print("  FAIL the manifest restore did not take (the app rewrote it)")
    return False


def alpaca_camera() -> str:
    """A camera that frames the alpaca WHERE THE MANIFEST PUTS IT."""
    doc = _read_manifest()
    pos = (11.75, 0.45, 12.55)
    for layer in doc["layers"]:
        if layer.get("name") == "alpaca" or layer.get("file", "").endswith("alpaca.vxw"):
            p = layer.get("pos", [0, 0, 0])
            pos = (p[0] + 0.2, 0.45, p[2] + 0.5)
            break
    return "%.2f,0.80,%.2f,%.2f,0.45,%.2f" % (pos[0] - 0.85, pos[2] - 2.1,
                                                 pos[0], pos[2])


def blob_centroid(a: bytes, b: bytes) -> tuple[float, float, int]:
    """The centroid and the size of the blob where the two frames differ."""
    tot_x = 0
    tot_y = 0
    cnt = 0
    for i in range(len(a) // 3):
        o = i * 3
        d = max(abs(a[o] - b[o]), abs(a[o + 1] - b[o + 1]), abs(a[o + 2] - b[o + 2]))
        if d > 6:
            tot_x += i % W
            tot_y += i // W
            cnt += 1
    if cnt == 0:
        return 0.0, 0.0, 0
    return tot_x / cnt, tot_y / cnt, cnt


def main() -> int:
    global OUTDIR
    OUTDIR = tempfile.mkdtemp(prefix="vf-fur-check-")
    print(f"fur_check: {APP}")
    save_micro()

    alpaca_cam = alpaca_camera()
    print(f"  alpaca camera: {alpaca_cam}")
    # The alpaca is a grazer: it walks, and a gate that measures a moving
    # animal measures the GAIT. The behaviour is cleared for the duration.
    set_behaviour("micro/alpaca.vxw", "")

    try:
        # ---- the instrument's noise floor, measured first ------------------
        render(os.path.join(OUTDIR, "a.ppm"), alpaca_cam, env_extra={"VF_FUR": "0"})
        render(os.path.join(OUTDIR, "b.ppm"), alpaca_cam, env_extra={"VF_FUR": "0"})
        _, _, off1 = read_ppm(os.path.join(OUTDIR, "a.ppm"))
        _, _, off2 = read_ppm(os.path.join(OUTDIR, "b.ppm"))
        n_noise, *_ = diff_stats(off1, off2, NOISE_MAX)
        print(f"  noise floor: {n_noise} pixels above {NOISE_MAX}/255 "
              f"(two identical renders)")
        # The floor is not ZERO and must not be: the scene's grass is animated
        # by the wall clock, so two renders of the SAME configuration differ
        # slightly. Every "unchanged" claim below is therefore stated against
        # this measured floor, not against zero.
        note(n_noise <= 200,
             f"the noise floor is small ({n_noise} stray pixels above "
             f"{NOISE_MAX}/255)")

        # ---- 1 + 2: the fur exists and is confined -------------------------
        log = render(os.path.join(OUTDIR, "on.ppm"), alpaca_cam)
        _, _, on = read_ppm(os.path.join(OUTDIR, "on.ppm"))
        m = re.search(r"fur: 1 object\(s\), (\d+) cards", log)
        note(m is not None and int(m.group(1)) > 1000 if m else False,
             f"the fur pass loaded cards (log: {m.group(0) if m else 'NO FUR LINE'})")
        cnt, minx, miny, maxx, maxy, box = diff_stats(on, off1, NOISE_MAX)
        note(cnt > 500,
             f"the fur changes the alpaca ({cnt} pixels above {NOISE_MAX}/255)")
        note(cnt > 0 and box > 0 and cnt / box > 0.15,
             f"the change is a compact blob (fill {cnt/max(box,1):.2f} of its bbox)")
        note(cnt < 0.20 * W * H,
             f"the change does not spread over the frame ({cnt} of {W*H} pixels)")
        note(cnt == 0 or (0 <= minx and maxx < W and 0 <= miny and maxy < H),
             "the change's bounding box is inside the frame")

        # ---- 2b: the coat is DETAILED, and it keeps the fleece's texture --
        # Two numbers, both about the coat being a coat and not a wash:
        #
        #   detail - the mean |difference| between horizontal neighbours. A
        #     coat whose strands are smeared into a haze has LITTLE of it;
        #     a coat with readable strands has more than the bare body.
        #   spread - the luma's standard deviation. A flat wash has the
        #     body's low spread; a coat that keeps the fleece's texture has
        #     a much higher one.
        #
        # The first check caught a BLUR once already: a large curl (0.45)
        # spreads a strand's ink over a wide band, and the coat's detail fell
        # to 12.7 - BELOW the bare body's 13.5. So the claim is directional:
        # the coat must be at least as detailed as the body under it, and it
        # must keep at least the body's texture contrast.
        def neighbour_diff(buf: bytes, x0: int, y0: int, x1: int, y1: int) -> float:
            tot = 0
            cnt = 0
            for y in range(y0, min(y1, H)):
                row = y * W
                for x in range(x0, min(x1, W) - 1):
                    o = (row + x) * 3
                    for c in range(3):
                        tot += abs(buf[o + c] - buf[o + c + 3])
                    cnt += 3
            return tot / max(cnt, 1)

        def luma_spread(buf: bytes, x0: int, y0: int, x1: int, y1: int) -> float:
            vals = []
            for y in range(y0, min(y1, H)):
                row = y * W
                for x in range(x0, min(x1, W)):
                    o = (row + x) * 3
                    vals.append((buf[o] * 299 + buf[o + 1] * 587 + buf[o + 2] * 114)
                                / 1000.0)
            m = sum(vals) / max(len(vals), 1)
            return (sum((v - m) ** 2 for v in vals) / max(len(vals), 1)) ** 0.5

        fur_det = neighbour_diff(on, 260, 100, 520, 480)
        body_det = neighbour_diff(off1, 260, 100, 520, 480)
        fur_spr = luma_spread(on, 260, 100, 520, 480)
        body_spr = luma_spread(off1, 260, 100, 520, 480)
        print(f"  detail: fur {fur_det:.2f} vs the bare body {body_det:.2f}   "
              f"spread: fur {fur_spr:.2f} vs {body_spr:.2f}")
        # The margin covers the run-to-run noise of the metric itself (the
        # scene's animated grass moves a little between renders).
        note(fur_det >= body_det - 0.5,
             f"the coat's strands are readable, not a haze ({fur_det:.2f} vs "
             f"the body's {body_det:.2f})")
        note(fur_spr >= body_spr,
             f"the coat keeps the fleece's texture ({fur_spr:.2f} vs the "
             f"body's {body_spr:.2f})")

        # ---- 3: the fur touches nothing else ------------------------------
        # The house view has no micro object in it at all, so the fur must
        # leave it EXACTLY as it found it. A DDA that walked off the object,
        # or a coverage that leaked, would show up here as a wash over the
        # whole frame - which no caption would call a bug.
        render(os.path.join(OUTDIR, "h_off.ppm"), HOUSE_CAM, env_extra={"VF_FUR": "0"})
        render(os.path.join(OUTDIR, "h_on.ppm"), HOUSE_CAM)
        _, _, h_off = read_ppm(os.path.join(OUTDIR, "h_off.ppm"))
        _, _, h_on = read_ppm(os.path.join(OUTDIR, "h_on.ppm"))
        c, *_ = diff_stats(h_on, h_off, NOISE_MAX)
        note(c == 0,
             f"the fur leaves the house view untouched ({c} pixels changed)")
        # The vase and the urn ride the SAME micro path as the alpaca and
        # carry no fur material, so the fur must not touch them. The camera
        # TARGETS them, so they sit in the middle of the frame: the check is
        # that the change does not reach the centre. (The material rule itself
        # is pinned by a unit test - "only a fur material grows a coat".)
        render(os.path.join(OUTDIR, "m_off.ppm"), MICRO_CAM, env_extra={"VF_FUR": "0"})
        render(os.path.join(OUTDIR, "m_on.ppm"), MICRO_CAM)
        _, _, m_off = read_ppm(os.path.join(OUTDIR, "m_off.ppm"))
        _, _, m_on = read_ppm(os.path.join(OUTDIR, "m_on.ppm"))
        centre = 0
        cminx, cminy, cmaxx, cmaxy = W, H, -1, -1
        for y in range(H):
            row = y * W
            for x in range(W):
                o = (row + x) * 3
                d = max(abs(m_on[o] - m_off[o]),
                        abs(m_on[o + 1] - m_off[o + 1]),
                        abs(m_on[o + 2] - m_off[o + 2]))
                if d > NOISE_MAX:
                    cminx, cmaxx = min(cminx, x), max(cmaxx, x)
                    cminy, cmaxy = min(cminy, y), max(cmaxy, y)
                    if abs(x - W // 2) <= 50 and abs(y - H // 2) <= 50:
                        centre += 1
        note(centre <= n_noise,
             f"the fur leaves the vase and the urn alone ({centre} pixels "
             f"changed in the middle of the frame, the floor is {n_noise})")

        # ---- 4: the fur is backend-independent ----------------------------
        render(os.path.join(OUTDIR, "svo_off.ppm"), alpaca_cam, mode="svo",
               env_extra={"VF_FUR": "0"})
        render(os.path.join(OUTDIR, "svo_on.ppm"), alpaca_cam, mode="svo")
        _, _, s_off = read_ppm(os.path.join(OUTDIR, "svo_off.ppm"))
        _, _, s_on = read_ppm(os.path.join(OUTDIR, "svo_on.ppm"))
        c, *_ = diff_stats(s_on, s_off, NOISE_MAX)
        note(c > 500,
             f"the fur is in the SVO reference backend too ({c} pixels changed)")

        # ---- 4b: the coat FOLLOWS the object it belongs to -----------------
        # The animal is turned 90 degrees about its base (the existing
        # rotate-preview hook) and the coat must turn with it. A coat left in
        # the rest pose would hang in the air where the animal used to be -
        # the same failure the autonomous WALK produces, and the walk cannot
        # be measured in a gate (the animal moves between two renders).
        #
        # The measurement: the coat's blob is the difference between the fur
        # on and the fur off, so its centroid says where the coat IS. Turned
        # 90 degrees, the centroid must MOVE.
        rot_env = {"VF_TEST_ROTATE_LIVE": "90,0,0",
                   "VF_ROTATE_LAYER": "micro/alpaca.vxw"}
        render(os.path.join(OUTDIR, "rot_off.ppm"), alpaca_cam,
               env_extra={"VF_FUR": "0"})
        render(os.path.join(OUTDIR, "rot_on.ppm"), alpaca_cam)
        render(os.path.join(OUTDIR, "rot90_off.ppm"), alpaca_cam,
               env_extra=dict(rot_env, VF_FUR="0"))
        render(os.path.join(OUTDIR, "rot90_on.ppm"), alpaca_cam, env_extra=rot_env)
        _, _, rot_off = read_ppm(os.path.join(OUTDIR, "rot_off.ppm"))
        _, _, rot_on = read_ppm(os.path.join(OUTDIR, "rot_on.ppm"))
        _, _, r90_off = read_ppm(os.path.join(OUTDIR, "rot90_off.ppm"))
        _, _, r90_on = read_ppm(os.path.join(OUTDIR, "rot90_on.ppm"))
        c0 = blob_centroid(rot_on, rot_off)
        c1 = blob_centroid(r90_on, r90_off)
        shift = ((c0[0] - c1[0]) ** 2 + (c0[1] - c1[1]) ** 2) ** 0.5
        print(f"  the coat's blob: {c0[0]:.0f},{c0[1]:.0f} upright, "
              f"{c1[0]:.0f},{c1[1]:.0f} turned 90 deg (shift {shift:.0f} px)")
        note(c0[2] > 500 and c1[2] > 500 and shift > 20.0,
             f"the coat turns with the animal (its blob moved {shift:.0f} px)")

        # ---- 5: the escape hatch ------------------------------------------
        log_off = render(os.path.join(OUTDIR, "esc.ppm"), alpaca_cam,
                         env_extra={"VF_FUR": "0"})
        note("fur: 1 object" not in log_off,
             "VF_FUR=0 loads no fur object at all (the pass never dispatches)")
        _, _, esc = read_ppm(os.path.join(OUTDIR, "esc.ppm"))
        c, *_ = diff_stats(esc, off1, NOISE_MAX)
        note(c <= n_noise,
             f"VF_FUR=0 renders the same frame as the fur-off reference "
             f"({c} pixels changed, the floor is {n_noise})")

    finally:
        if not restore_micro():
            failures.append("the manifest restore did not take")
        for name in os.listdir(OUTDIR):
            try:
                os.remove(os.path.join(OUTDIR, name))
            except OSError:
                pass
        os.rmdir(OUTDIR)

    print()
    if failures:
        print(f"fur_check: {len(failures)} FAILED")
        for f in failures:
            print("  - " + f)
        return 1
    print("fur_check: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
