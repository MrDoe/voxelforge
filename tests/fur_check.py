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
     SAME configuration with the animal's pose PINNED (see FREEZE), so the
     floor is a determinism test - it must be ZERO pixels - and the signal
     has to clear it.
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
# The authored manifest lives in the active scenery folder (assets/ is only
# the shared texture/model library now). Pin the hermetic default and use the
# same path the app resolves for every render in this gate.
_SCENE = os.environ.setdefault("VF_SCENERY", "LakesideHamlet")
MANIFEST = (os.path.join(_SCENE, "world.json")
            if ("/" in _SCENE or "\\" in _SCENE)
            else os.path.join(ROOT, "scenery", _SCENE, "world.json"))

# The noise floor of a same-configuration pair. With the pose PINNED (see
# FREEZE) a headless `--shot` is byte-reproducible, so this is a determinism
# test and not a tolerance: the only thing that may differ is a race or an
# uninitialised read in the frame.
NOISE_MAX = 6

# The pose every render in this gate uses: name, offset, heading, gait, graze,
# clipTime. Chosen once and never varied, so every arm compares the SAME animal.
FREEZE = "alpaca:0,0,0:0:0:0:0.32"

failures: list[str] = []


def note(ok: bool, msg: str) -> None:
    print(("  ok   " if ok else "  FAIL ") + msg)
    if not ok:
        failures.append(msg)


def render(out: str, cam: str, mode: str = "", env_extra: dict | None = None,
           args_extra: list | None = None) -> str:
    cmd = [APP, "--shot", out, "--cam", cam, "--width", str(W), "--height", str(H)]
    if mode:
        cmd += ["--mode", mode]
    if args_extra:
        cmd += args_extra
    env = os.environ.copy()
    # Never touch the live-edit overlay file: this is a read-only measurement.
    env["VF_OVERLAY_PATH"] = os.path.join(OUTDIR, "overlay.vxw")
    # PIN THE POSE. The alpaca is a grazer, so it walks, and a gate that
    # measures a moving animal measures the GAIT: two renders of the SHIPPED
    # scene differ by ~14,000 px at this camera, all of it walk phase, which is
    # why every metric below used to be stated against a floor of thousands.
    # The hook pins the transform, the gait phase and the graze blend while
    # keeping the ground clamp, so the pair becomes a true determinism test and
    # every arm compares the same pose.
    #
    # This replaced clearing `behaviour` in the manifest, which did NOT stand
    # the animal still: the layer also carries `"command": "auto"`, and that
    # keeps it walking on its own. It also removes a gate's need to WRITE a
    # tracked file - the app rewrites world.json from its own layer list, so a
    # write can land after the gate's restore (that is how the shipped alpaca
    # lost its `behaviour` twice).
    env["VF_TEST_BEHAVIOUR_FREEZE"] = FREEZE
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
    # The pose is pinned through the environment (see render()), so the gate
    # never writes the manifest. `save_micro`/`restore_micro` stay as a GUARD:
    # they verify the shipped scene came through the run unchanged, which is
    # the check AGENTS.md asks for (the app rewrites world.json every run).
    save_micro()

    alpaca_cam = alpaca_camera()
    print(f"  alpaca camera: {alpaca_cam}")

    try:
        # ---- the instrument's noise floor, measured first ------------------
        render(os.path.join(OUTDIR, "a.ppm"), alpaca_cam, env_extra={"VF_FUR": "0"})
        render(os.path.join(OUTDIR, "b.ppm"), alpaca_cam, env_extra={"VF_FUR": "0"})
        _, _, off1 = read_ppm(os.path.join(OUTDIR, "a.ppm"))
        _, _, off2 = read_ppm(os.path.join(OUTDIR, "b.ppm"))
        n_noise, *_ = diff_stats(off1, off2, NOISE_MAX)
        print(f"  noise floor: {n_noise} pixels above {NOISE_MAX}/255 "
              f"(two identical renders)")
        # The floor must be ZERO. The pose is pinned, so two renders of the SAME
        # configuration have the same clock, the same geometry and the same
        # shading inputs: any difference is a race or an uninitialised read in
        # the frame. It used to be 12,000-14,000 px, all of it walk phase - see
        # the FREEZE note in render(). Every "unchanged" claim below is stated
        # against this floor, so a tolerance here would hide exactly the
        # G-buffer double-writer race the mesh raster path had.
        note(n_noise == 0,
             f"the noise floor is ZERO ({n_noise} stray pixels above "
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
        # Two numbers, both measured INSIDE THE COAT'S OWN FOOTPRINT (the
        # pixels the fur changed, dilated 3 px so the coat's rim counts):
        #
        #   detail - the mean |difference| between horizontal neighbours. A
        #     coat whose strands are smeared into a haze has LITTLE of it;
        #     a coat with readable strands has more than the bare body.
        #   spread - the luma's standard deviation. A flat wash has almost
        #     none; a coat that keeps the fleece's texture keeps most of the
        #     body's.
        #
        # WHY THE FOOTPRINT AND NOT A FIXED RECT. Both metrics used to run
        # over (260,100)-(520,480), a 98,800 px box of which the coat covers
        # 23.7 %: the numbers were mostly SCENERY, so the check measured the
        # frame rather than the coat. Measured on the shipped scene, footprints
        # differ by 8 % (detail) where the old rect differed by 2 %.
        #
        # The detail check caught a BLUR once already: a large curl (0.45)
        # spreads a strand's ink over a wide band and the coat's detail fell
        # BELOW the bare body's. So its claim is directional: the coat must be
        # at least as detailed as the body under it.
        #
        # The spread check's claim is weaker than it looks, ON PURPOSE. A
        # translucent layer AVERAGES the dark crevices it covers, so it
        # COMPRESSES large-scale contrast even when it adds texture: measured
        # 22.90 against the bare body's 28.35 (ratio 0.81) on the shipped
        # scene. Requiring spread >= the body's was only ever true while the
        # mesh's AO sampled INWARD and the bare body was flat; the orientation
        # fix points it outward, which raises the body's own contrast. So the
        # bound is a floor that a flat wash fails (it would sit near zero) and
        # a textured coat passes with margin.
        def coat_footprint(dilate: int = 3) -> bytearray:
            m = bytearray(len(on) // 3)
            for i in range(len(m)):
                o = i * 3
                if max(abs(on[o] - off1[o]), abs(on[o + 1] - off1[o + 1]),
                       abs(on[o + 2] - off1[o + 2])) > NOISE_MAX:
                    m[i] = 1
            for _ in range(dilate):
                g = bytearray(m)
                for y in range(H):
                    row = y * W
                    for x in range(W):
                        i = row + x
                        if m[i]:
                            continue
                        if ((x > 0 and m[i - 1]) or (x + 1 < W and m[i + 1])
                                or (y > 0 and m[i - W]) or (y + 1 < H and m[i + W])):
                            g[i] = 1
                m = g
            return m

        def neighbour_diff(buf: bytes, mask: bytearray) -> float:
            tot = 0
            cnt = 0
            for y in range(H):
                row = y * W
                for x in range(W - 1):
                    i = row + x
                    if not (mask[i] and mask[i + 1]):
                        continue
                    o = i * 3
                    for c in range(3):
                        tot += abs(buf[o + c] - buf[o + c + 3])
                    cnt += 3
            return tot / max(cnt, 1)

        def luma_spread(buf: bytes, mask: bytearray) -> float:
            vals = []
            for i in range(len(mask)):
                if not mask[i]:
                    continue
                o = i * 3
                vals.append((buf[o] * 299 + buf[o + 1] * 587 + buf[o + 2] * 114)
                            / 1000.0)
            m = sum(vals) / max(len(vals), 1)
            return (sum((v - m) ** 2 for v in vals) / max(len(vals), 1)) ** 0.5

        fp = coat_footprint()
        fur_det = neighbour_diff(on, fp)
        body_det = neighbour_diff(off1, fp)
        fur_spr = luma_spread(on, fp)
        body_spr = luma_spread(off1, fp)
        print(f"  footprint: {sum(fp)} px   detail: fur {fur_det:.2f} vs the "
              f"bare body {body_det:.2f}   spread: fur {fur_spr:.2f} vs "
              f"{body_spr:.2f}")
        # Both frames are the SAME pinned pose, so the comparison is between
        # the coat and the body under it and nothing else.
        note(fur_det >= body_det,
             f"the coat's strands are readable, not a haze ({fur_det:.2f} vs "
             f"the body's {body_det:.2f})")
        note(fur_spr >= 0.7 * body_spr,
             f"the coat keeps the fleece's texture ({fur_spr:.2f} vs the "
             f"body's {body_spr:.2f}, floor {0.7*body_spr:.2f})")

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

        # ---- 6: backlit translucency --------------------------------------
        # Real fleece transmits light when the sun is behind it; a Lambert-only
        # coat reads as an opaque cut-out. These three renders use a BACKLIT
        # sun (azim 22 puts the sun behind the animal from this camera) and
        # toggle VF_FUR_SSS. The strength is a composite uniform, so the two
        # arms are otherwise byte-identical - the difference is ONLY the glow.
        backlit = ["--sun", "20", "22"]
        render(os.path.join(OUTDIR, "sss0.ppm"), alpaca_cam,
               args_extra=backlit, env_extra={"VF_FUR_SSS": "0"})
        render(os.path.join(OUTDIR, "sss1.ppm"), alpaca_cam,
               args_extra=backlit, env_extra={"VF_FUR_SSS": "0.8"})
        _, _, sss0 = read_ppm(os.path.join(OUTDIR, "sss0.ppm"))
        _, _, sss1 = read_ppm(os.path.join(OUTDIR, "sss1.ppm"))
        sss_cnt, *_ = diff_stats(sss1, sss0, NOISE_MAX)
        note(sss_cnt > 3000,
             f"backlit translucency lights the fleece ({sss_cnt} pixels)")
        # The glow must WARM the coat (R-B up) and brighten it - a red/blue
        # wash with no luma change would be a tint, not subsurface scattering.
        def coat_luma_rb(buf: bytes, mask: bytes) -> tuple[float, float]:
            ls = rs = 0.0
            n = 0
            for i in range(len(mask)):
                if not mask[i]:
                    continue
                o = i * 3
                ls += 0.299 * buf[o] + 0.587 * buf[o + 1] + 0.114 * buf[o + 2]
                rs += buf[o] - buf[o + 2]
                n += 1
            return (ls / max(n, 1), rs / max(n, 1))
        glow = bytearray(len(sss0) // 3)
        for i in range(len(glow)):
            o = i * 3
            if max(abs(sss1[o] - sss0[o]), abs(sss1[o + 1] - sss0[o + 1]),
                   abs(sss1[o + 2] - sss0[o + 2])) > NOISE_MAX:
                glow[i] = 1
        l0, rb0 = coat_luma_rb(sss0, glow)
        l1, rb1 = coat_luma_rb(sss1, glow)
        note(l1 > l0 + 5.0 and rb1 > rb0 + 2.0,
             f"the glow warms and brightens the fleece "
             f"(luma {l0:.0f}->{l1:.0f}, R-B {rb0:.0f}->{rb1:.0f})")
        # Confinement: the SSS uniform must not touch a view with no fur in it.
        render(os.path.join(OUTDIR, "hsss0.ppm"), HOUSE_CAM,
               args_extra=backlit, env_extra={"VF_FUR_SSS": "0"})
        render(os.path.join(OUTDIR, "hsss1.ppm"), HOUSE_CAM,
               args_extra=backlit, env_extra={"VF_FUR_SSS": "0.8"})
        _, _, h0 = read_ppm(os.path.join(OUTDIR, "hsss0.ppm"))
        _, _, h1 = read_ppm(os.path.join(OUTDIR, "hsss1.ppm"))
        hc, *_ = diff_stats(h1, h0, NOISE_MAX)
        note(hc == 0,
             f"the glow touches nothing without fur in view ({hc} px on the house)")
        # The DEFAULT sun is side/back to these cameras: shipping shots must be
        # byte-identical with SSS on, so no existing gate or reference moves.
        render(os.path.join(OUTDIR, "dsss0.ppm"), alpaca_cam,
               env_extra={"VF_FUR_SSS": "0"})
        render(os.path.join(OUTDIR, "dsss1.ppm"), alpaca_cam,
               env_extra={"VF_FUR_SSS": "0.8"})
        _, _, d0 = read_ppm(os.path.join(OUTDIR, "dsss0.ppm"))
        _, _, d1 = read_ppm(os.path.join(OUTDIR, "dsss1.ppm"))
        dc, *_ = diff_stats(d1, d0, NOISE_MAX)
        note(dc == 0,
             f"the default sun's reference frame is unchanged ({dc} px)")

        # ---- 7: per-frame sun-space micro shadow (Workstream B, default off)
        # At a grazing sun the PCF path must reproduce the field-march verdict
        # (parity) and be deterministic (two ON renders byte-identical).
        backlit_low = ["--sun", "3", "100"]
        render(os.path.join(OUTDIR, "sun_off.ppm"), alpaca_cam,
               args_extra=backlit_low, env_extra={"VF_MICRO_SUNSHADOW": "0"})
        render(os.path.join(OUTDIR, "sun_on.ppm"), alpaca_cam,
               args_extra=backlit_low, env_extra={"VF_MICRO_SUNSHADOW": "1"})
        render(os.path.join(OUTDIR, "sun_on2.ppm"), alpaca_cam,
               args_extra=backlit_low, env_extra={"VF_MICRO_SUNSHADOW": "1"})
        _, _, s0 = read_ppm(os.path.join(OUTDIR, "sun_off.ppm"))
        _, _, s1 = read_ppm(os.path.join(OUTDIR, "sun_on.ppm"))
        _, _, s2 = read_ppm(os.path.join(OUTDIR, "sun_on2.ppm"))
        sc, *_ = diff_stats(s1, s0, NOISE_MAX)
        note(sc == 0,
             f"sun-space shadow matches the field verdict at grazing sun ({sc} px)")
        sd, *_ = diff_stats(s2, s1, NOISE_MAX)
        note(sd == 0,
             f"sun-space shadow is deterministic ({sd} px)")

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
