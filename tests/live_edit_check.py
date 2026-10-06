#!/usr/bin/env python3
"""Live-edit (M1-M3) regression guard for voxelforge.

Renders the close-up view several times and asserts:
  - an untouched render is sane (coverage / sky probe),
  - one live store edit (VF_TEST_EDIT) at a known cell for each brush mode -
    raise (Add), carve, delete, paint and object-surface smooth - logs dirty
    chunks + patched
    surfels and changes a visible but bounded fraction of pixels, without
    wrecking the frame (both backends for raise; splat for the store-only
    delete/paint modes, which have no record-layer form),
  - an object-surface Smooth pick takes the surface-axis relaxation (log
    "smooth object:") and never the terrain column path, and the patched
    splats change visible pixels,
  - a live-patched chunk keeps its deterministic micro-detail tail (run surfel
    count with VF_MICRO on >> off for the same edit),
  - the carve hover preview tints warm and the add preview tints the growth's
    footprint green (VF_TEST_BRUSH, no edit applied), the Depth slider being
    visible at all and the SVO backend showing the same preview,
  - a 1-voxel Add/Carve stamps EXACTLY one cell (the per-voxel sculpt mode),
  - Undo is a TRUE INVERSE of the edit - the post-undo surfel run must equal
    the add-only run - on a chunk carrying object geometry (terrain-only edits
    cannot catch a too-wide refresh),
  - undo and "Clear live edits" remove live geometry again (an undone stroke
    and a cleared world must stop rendering the removed material, and the
    cleared overlay file must be gone).

Usage: live_edit_check.py <path-to-voxelforge-binary>
Stdlib only - parses the PPM output directly.
"""
import os
import re
import subprocess
import sys
import tempfile

W, H = 480, 270
FAST = os.environ.get("VF_FAST_TESTS") == "1"
# close view of the edited terrain cell (432,509,452): the dome fills a
# meaningful part of the frame in both backends
CAM = ["-5.5", "1.2", "-3.5", "-8", "0.4", "-6"]
# terrain surface cell in front of the hero camera (world ~(-8, -0.15, -6))
CELL = "432,509,452"
EDIT = CELL + ",raise"
# hero view (the reference house.jpeg camera): its centre ray lands on the
# cabin's underside at this surface cell, so an object-pick Smooth exercises
# the object-surface relaxation end to end (object branch, live splat patch).
HERO_CAM = ["1.0", "2.0", "1.5", "5.3", "1.0", "11.3"]
OBJECT_CELL = "557,523,607"
# dock plank next to open water (y ~ -0.35) + the water camera from
# visual_check: a big brush here reaches over the water surface
SHORE_CELL = "562,508,582"
WATER_CAM = ["8.5", "0.6", "8.2", "4.5", "-1.1", "6.8"]
# flat open ground beside the river (surface ~-0.45, plane -0.9): a 6 m ball
# delete digs an open pit whose floor is submerged
PIT_CELL = "512,507,512"
PIT_CAM = ["0", "6", "0", "0", "-0.5", "0"]
# shore channel that reaches the river (surface ~-0.3, the river edge ~0.4 m
# away): the carve digs below the plane and the channel water must read exactly
# like the open river water in the same frame (one fixed level, one shader)
CHANNEL_CELL = "512,508,542"
CHANNEL_CAM = ["0", "2", "-1.5", "0", "-1", "4"]


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
    warm_r = warm_b = warm_g = 0
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
                warm_g += b[3 * k + 1]
    return d / n, warm_r, warm_b, warm_g


def render(binary, out, extra_env=None, mode=None, cam=None, overlay=False):
    env = dict(os.environ)
    # hermetic by default: skip restoring a saved live-edit overlay so the
    # comparison isolates this run's own edits (a session's painting would
    # otherwise dominate both frames). overlay=True reads the file the run's
    # own VF_OVERLAY_PATH points at (undo/clear checks).
    if not overlay:
        env["VF_NO_OVERLAY"] = "1"
    if extra_env:
        env.update(extra_env)
    # A guard that is only applied at SOME call sites is not a guard: a
    # stamping run (VF_TEST_EDIT/STROKE/BRUSH) re-serialises the overlay even
    # with VF_NO_OVERLAY=1, because that flag suppresses only the LOAD, and
    # with no VF_OVERLAY_PATH the app writes assets/runtime_edits.vxw - the
    # user's live-edit session, which is gitignored and therefore unrecoverable.
    # Measured 2026-10-06: a "clean env" run (which strips an ambient
    # VF_OVERLAY_PATH that had been masking this) rewrote a 64,907,610 B
    # session to 980,100 B with no warning. So default the path into this run's
    # own tmp dir, next to the frame it belongs to; an explicit VF_OVERLAY_PATH
    # from the caller still wins, which is what the per-voxel and undo/clear
    # checks rely on.
    if not env.get("VF_OVERLAY_PATH"):
        stem = os.path.splitext(os.path.basename(out))[0]
        env["VF_OVERLAY_PATH"] = os.path.join(
            os.path.dirname(out) or ".", stem + "_overlay.vxw")
    cmd = [
        binary, "--shot", out,
        "--width", str(W), "--height", str(H),
        "--cam", *(cam or CAM),
    ]
    if mode:
        cmd += ["--mode", mode]
    return subprocess.run(cmd, capture_output=True, text=True, timeout=300, env=env)


def is_water(r, g, b):
    # blue clearly dominant over red AND green: excludes fog-washed grey
    # surfaces that the old loose rule (b > r+12) misclassified as water
    return b > r + 25 and b > g + 8 and b > 120


def flood_pixels(dry_path, wet_path):
    """(changed, surface) between an unflooded and a flooded frame: a surface
    pixel got brighter and blue-biased (the water-plane splats)."""
    w, h, a = read_ppm(dry_path)
    _, _, b = read_ppm(wet_path)
    changed = surface = 0
    for k in range(w * h):
        if max(abs(a[3 * k + i] - b[3 * k + i]) for i in range(3)) > 12:
            changed += 1
            la = (a[3 * k] + a[3 * k + 1] + a[3 * k + 2]) / 3.0
            lb = (b[3 * k] + b[3 * k + 1] + b[3 * k + 2]) / 3.0
            if lb > la + 15 and b[3 * k + 2] >= b[3 * k]:
                surface += 1
    return changed, surface


def check_per_voxel(binary, tmp, failures):
    """A 1-voxel brush is the per-voxel sculpt mode and must touch EXACTLY one
    cell. This pins two ways that used to break: the old radius clamp had a
    0.1 m floor, so even a "1 voxel" brush rasterized 3 cells across, and the
    dome/cylinder volume shapes would also pick up the neighbouring cell along
    the normal. The cell count in the stamp log is the strong, resolution-
    independent signal (one 0.1 m voxel is near the ASCII/render limit), so
    there is deliberately no pixel-diff lower bound here - only a sanity cap.
    """
    for mode in ("add", "carve"):
        out = os.path.join(tmp, f"pervoxel_{mode}.ppm")
        r = render(binary, out, {
            "VF_TEST_EDIT": f"{CELL},{mode}",
            "VF_EDIT_DIAM": "0.1",  # one voxel
            "VF_OVERLAY_PATH": os.path.join(tmp, f"pervoxel_{mode}.vxw"),
        })
        logs = (r.stdout or "") + (r.stderr or "")
        if r.returncode != 0 or not os.path.exists(out):
            failures.append(f"per-voxel {mode}: render failed")
            continue
        m = re.search(r"live edit: (\d+) cells", logs)
        if not m:
            failures.append(f"per-voxel {mode}: no 'live edit: N cells' log")
            continue
        cells = int(m.group(1))
        w, h, img = read_ppm(out)
        s = stats(w, h, img)
        print(f"[per-voxel] {mode}: {cells} cells  coverage {s['obj']*100:.1f}%  "
              f"black {s['black']*100:.2f}%")
        if cells != 1:
            failures.append(f"per-voxel {mode}: stamped {cells} cells, expected 1 "
                            "(the brush is not per-voxel)")
        if s["black"] > 0.09:
            failures.append(f"per-voxel {mode}: black-in-silhouette "
                            f"{s['black']*100:.2f}%")


def check_object_undo_surgical(binary, tmp, failures):
    """Undo must be a TRUE INVERSE of the edit, checked on a chunk that carries
    object geometry - which the terrain-only checks above cannot catch.

    Regression: undoEdit forced the wide kExactStampMargin (+-12) on every undo
    because Smooth needs it, so undoing a ONE-voxel edit re-derived a 25^3 box
    from the store - 1310 parents + 599 edge bridges, against 52 bridges in the
    whole chunk from the bake. The store's object surface/crease classification
    does not match the bake's, so widening the region DIVERGES instead of
    converging: the chunk lost its baked surfels and nearby structure read
    hollow with splats in the wrong place. Measured 2.571% of pixels vs 0.185%
    for the add itself; the same edit on terrain moved 0.02%, which is why a
    terrain-only suite can never fail on it.

    The primary assertion is CONTENT-FREE and needs no threshold: the run split
    after add+undo must EQUAL the run split after the add alone. That is what
    makes the undo a true inverse, it cannot drift with a re-authored hamlet,
    and the bug fails it outright (4157+602 against 4298+52). The edge bound and
    the pixel diff are kept as secondaries because they fail louder and earlier.

    CELL is a solid, object-owned cell (hamlet structure). It is asserted, not
    assumed: three premise asserts run before any outcome is judged - the stamp
    happened, the region holds real baked geometry, and the pick is an object -
    because a check that silently stops testing is worse than one that misfires.
    The true-inverse comparison is content-free and cannot drift when the hamlet
    is re-authored; these three only have to notice that it went inert.
    """
    wall = "562,524,607"
    cam = ["5", "2.6", "2.0", "5", "1.8", "10"]
    base = os.path.join(tmp, "objundo_base.ppm")
    added = os.path.join(tmp, "objundo_added.ppm")
    shot = os.path.join(tmp, "objundo.ppm")
    render(binary, base, {"VF_MICRO": "0"}, cam=cam)
    # VF_TRACE so the forward stamp's line is not suppressed while "dragging";
    # the undo line prints either way.
    r = render(binary, added, {
        "VF_MICRO": "0", "VF_TRACE": "1",
        "VF_TEST_STROKE": f"{wall},1,add", "VF_EDIT_DIAM": "0.1",
    }, cam=cam)
    r2 = render(binary, shot, {
        "VF_MICRO": "0", "VF_TRACE": "1",
        "VF_TEST_STROKE": f"{wall},1,add",   # same run: undo the in-run stroke
        "VF_EDIT_DIAM": "0.1",               # one voxel
        "VF_TEST_UNDO": "1",
    }, cam=cam)
    logs = (r.stdout or "") + (r.stderr or "") + (r2.stdout or "") + (r2.stderr or "")
    if r.returncode != 0 or r2.returncode != 0 or not os.path.exists(shot):
        failures.append("object undo: render failed")
        return

    def split(who):
        m = re.search(r"live edit" + who + r": .*?run surfels "
                      r"\((\d+) parents \+ (\d+) edges", logs, re.S)
        return (int(m.group(1)), int(m.group(2))) if m else None

    # NON-VACUITY FIRST: a check that silently stops testing is worse than one
    # that misfires, because a false alarm gets investigated and a vacuous
    # pass gets trusted. If CELL stops being solid the add never happens, both
    # splits vanish or stay tiny, and without this the check would go green
    # having verified nothing at all. Same "1 cells" pattern as check_per_voxel.
    ms = re.search(r"live edit undo: (\d+) cells", logs)
    if not ms:
        failures.append(f"object undo: no stamp in the log - CELL {wall} is "
                        "stale (not solid or not editable); update the "
                        "coordinate in check_object_undo_surgical")
        return
    if int(ms.group(1)) != 1:
        failures.append(f"object undo: expected a 1-voxel stamp, log says "
                        f"{ms.group(1)} cells")
    # It must land on an OBJECT cell, or the check silently degrades to the
    # terrain class that provably cannot catch this bug (0.02% there vs 0.18%
    # here, on the same one-voxel add). This is only trustworthy because the
    # headless hook now resolves pick ownership from the load-time oracle: it
    # used to leave object=false, so this assertion read "terrain" for a cabin
    # wall and would have been permanently red for the wrong reason.
    pick = re.search(r"live edit undo: .*?pick (\w+)", logs, re.S)
    print(f"[object-undo] pick: {pick.group(1) if pick else '?'}")
    if not pick or pick.group(1) != "object":
        failures.append(f"object undo: the stamp did not land on an OBJECT cell "
                        f"(pick={pick.group(1) if pick else '?'}) - CELL {wall} "
                        "is not object-owned, so this check would be testing the "
                        "terrain class, which cannot catch this bug")
    # The stamp succeeding is not enough: a headless stamp happily writes a
    # voxel into OPEN AIR, so a stale coordinate still reports "1 cells" and
    # would then fail the comparisons below for the wrong reason (measured: an
    # air coordinate gave 1+1 vs 0+0 and blamed the undo). Require the region
    # to hold real baked geometry - there is nothing for the inverse to
    # preserve otherwise, so the comparison cannot mean anything. A genuine
    # object chunk has thousands.
    if not re.search(r"run surfels \(\d{3,} parents", logs):
        failures.append(f"object undo: the edited chunk has almost no baked "
                        "surfels - CELL is stale or in open air, so this check "
                        "would compare two empty regions and pass vacuously")
        return

    before, after = split(""), split(" undo")
    if before is None or after is None:
        failures.append("object undo: no run-split log to compare")
        return
    print(f"[object-undo] run after add {before[0]} parents + {before[1]} edges; "
          f"after add+undo {after[0]} parents + {after[1]} edges")
    if after != before:
        failures.append(f"object undo: run split {after[0]}+{after[1]} does not "
                        f"match the add-only split {before[0]}+{before[1]} - the "
                        "undo re-derived geometry the edit never touched (a "
                        "wider margin DIVERGES from the bake, it does not "
                        "converge on it)")
    # secondary: a wide refresh re-derives hundreds of edge bridges where the
    # chunk legitimately has ~52. Pixel-independent, so it still fires when the
    # misplacement happens to be subtle.
    if after[1] > 200:
        failures.append(f"object undo: {after[1]} edge bridges for a one-voxel "
                        "undo - a wide margin re-derived object geometry that "
                        "does not match the bake")
    w, h, a = read_ppm(base)
    _, _, b = read_ppm(shot)
    d = diff_stats(w, h, a, w, h, b)[0]
    print(f"[object-undo] post-undo diff vs untouched {d*100:.3f}%")
    if d > 0.005:
        failures.append(f"object undo: changed {d*100:.2f}% of pixels that the "
                        "edit never touched")


def check_water_fill(binary, tmp, failures):
    """The water is one fixed-level plane: a dug volume below LEVEL shows the
    same water as the river, with no per-column bookkeeping. A/B against
    VF_SPLAT_NOWATER=1 proves the pit/channel is water; the channel case also
    pins the colour parity with the open water in the same frame (a stale
    height texture used to shade carved water as a thin foam-washed sheet)."""
    # an open pit below the plane: delete a 6 m ball in flat ground beside the
    # river (surface ~-0.45, water plane -0.9, so the floor ends up submerged)
    pit_env = {"VF_TEST_EDIT": f"{PIT_CELL},delete", "VF_EDIT_DIAM": "6.0"}
    dry = os.path.join(tmp, "water_noflood.ppm")
    wet = os.path.join(tmp, "water_flood.ppm")
    r = render(binary, wet, pit_env, mode=None, cam=PIT_CAM)
    if r.returncode != 0 or not os.path.exists(wet):
        failures.append("water: pit render failed")
        return
    r1 = render(binary, dry, dict(pit_env, VF_SPLAT_NOWATER="1"), mode=None,
                cam=PIT_CAM)
    if r1.returncode != 0 or not os.path.exists(dry):
        failures.append("water: pit no-water render failed")
        return
    changed, surface = flood_pixels(dry, wet)
    print(f"[water] dug pit vs no-water changed {changed} px "
          f"({changed/(W*H)*100:.1f}%), {surface} read as the water surface")
    if changed < W * H * 0.01:
        failures.append(f"water: pit water is invisible ({changed} px changed)")
    if surface < 200:
        failures.append(f"water: pit water did not read as water ({surface} surface px)")

    # colour parity: carve a channel that reaches the river. Existing water
    # pixels must keep their colour and the newly exposed water (only below the
    # level) must match them - one level, one material, one shader.
    cbase = os.path.join(tmp, "water_channel_base.ppm")
    cchan = os.path.join(tmp, "water_channel.ppm")
    r0 = render(binary, cbase, mode=None, cam=CHANNEL_CAM)
    r1 = render(binary, cchan,
                {"VF_TEST_EDIT": f"{CHANNEL_CELL},carve", "VF_EDIT_DIAM": "4.0",
                 "VF_EDIT_DEPTH": "2.0"}, mode=None, cam=CHANNEL_CAM)
    if r1.returncode != 0 or not os.path.exists(cchan) or not os.path.exists(cbase):
        failures.append("water: channel renders failed")
        return
    w, h, a = read_ppm(cbase)
    _, _, b = read_ppm(cchan)
    old_water, new_water = [], []
    for k in range(w * h):
        c0 = (a[3 * k], a[3 * k + 1], a[3 * k + 2])
        c1 = (b[3 * k], b[3 * k + 1], b[3 * k + 2])
        if is_water(*c0):
            old_water.append(c1)   # existing water, carved frame
        elif is_water(*c1):
            new_water.append(c1)   # water only the dig exposed
    print(f"[water] channel water: {len(old_water)} existing px, "
          f"{len(new_water)} newly exposed px")
    if len(old_water) < 200:
        failures.append("water: channel frame lost the open water")
    if len(new_water) < 250:
        failures.append(f"water: carve exposed too little water ({len(new_water)} px)")
    if old_water and new_water:
        mo = [sum(c[i] for c in old_water) / len(old_water) for i in range(3)]
        mn = [sum(c[i] for c in new_water) / len(new_water) for i in range(3)]
        print(f"[water] channel colour existing ({mo[0]:.0f},{mo[1]:.0f},{mo[2]:.0f}) "
              f"vs new ({mn[0]:.0f},{mn[1]:.0f},{mn[2]:.0f})")
        if max(abs(mo[i] - mn[i]) for i in range(3)) > 12:
            failures.append(
                "water: carved water does not match the open water colour "
                f"({mo[0]:.0f},{mo[1]:.0f},{mo[2]:.0f} vs "
                f"{mn[0]:.0f},{mn[1]:.0f},{mn[2]:.0f})")
    else:
        failures.append("water: channel colour comparison had no water")

    # subtractive hover tint leaves the water surface alone
    wbase = os.path.join(tmp, "water_base.ppm")
    wprev = os.path.join(tmp, "water_prev.ppm")
    r0 = render(binary, wbase, mode=None, cam=WATER_CAM)
    r1 = render(binary, wprev,
                {"VF_TEST_BRUSH": f"{SHORE_CELL},delete", "VF_EDIT_DIAM": "6.0"},
                mode=None, cam=WATER_CAM)
    if r1.returncode != 0 or not os.path.exists(wprev):
        failures.append("water: shore preview render failed")
        return
    w, h, a = read_ppm(wbase)
    _, _, b = read_ppm(wprev)
    tinted = water = 0
    for k in range(w * h):
        if max(abs(a[3 * k + i] - b[3 * k + i]) for i in range(3)) > 12:
            tinted += 1
            if is_water(a[3 * k], a[3 * k + 1], a[3 * k + 2]):
                water += 1
    print(f"[water] shore preview tinted {tinted} px, water-plane pixels tinted {water}")
    if tinted == 0:
        failures.append("water: shore preview tinted nothing")
    # The intent is "the water PLANE is never tinted". A handful of pixels
    # along the shoreline flip between is_water classes when a shore splat is
    # tinted (the heuristic reads the blended edge colour, not the plane), so
    # allow a small boundary margin - 2-3 px on the current scene, both on
    # this tree and on the pre-texture baseline. A real leak tints thousands.
    if water > 25 and water > 0.002 * tinted:
        failures.append(f"water: subtractive preview tinted {water} water pixels")


def check_micro_persistence(binary, tmp, failures, splat_log=None):
    """A live-patched chunk must keep its micro-detail tail: the same edit run
    with micros enabled logs substantially more run surfels than the same run
    with VF_MICRO=0 (the live editor regenerates the bake's hash-driven
    children instead of dropping them until the next full reload)."""
    def parse_surfels(logs):
        if not logs or "live edit:" not in logs:
            return None
        seg = logs.split("live edit:")[-1]
        if "run surfels" not in seg:
            return None
        try:
            return int(seg.split("run surfels")[0].split(",")[-1])
        except ValueError:
            return None

    def run_surfels(tag, extra_env):
        out = os.path.join(tmp, f"micro_{tag}.ppm")
        r = render(binary, out, extra_env)
        return parse_surfels((r.stdout or "") + (r.stderr or ""))

    # the splat pair above already rendered the same edit with micros on
    on = parse_surfels(splat_log)
    if on is None:
        on = run_surfels("on", {"VF_TEST_EDIT": EDIT})
    off = run_surfels("off", {"VF_TEST_EDIT": EDIT, "VF_MICRO": "0"})
    if on is None or off is None:
        failures.append("micro: live edit never logged a run surfel count")
        return
    print(f"[micro] patched run surfels with micros {on} vs without {off}")
    if on <= off * 1.1:
        failures.append(
            f"micro: live patch dropped its micro tail ({on} vs {off} surfels)")


def check_preview(binary, tmp, failures, baseline):
    """Hover previews: VF_TEST_BRUSH activates the edit tool with a brush
    volume but applies NO edit, so the only frame difference is the tint over
    the splats the brush would affect. Carve tints warm,
    Add tints the growth footprint green (the volume the raised shell buries),
    and Smooth tints its conservative terrain footprint blue.

    Renders its own untouched baseline when one is not already on disk, so this
    function is runnable standalone via `--only preview`."""
    if not os.path.exists(baseline):
        r0 = render(binary, baseline, {"VF_NO_OVERLAY": "1"}, mode=None)
        if r0.returncode != 0 or not os.path.exists(baseline):
            failures.append("preview: baseline render failed")
            return
    w, h, a = read_ppm(baseline)

    def preview(tag, env, warm=None, green=None, blue=None):
        prev = os.path.join(tmp, f"preview_{tag}.ppm")
        r1 = render(binary, prev, env, mode=None)
        if r1.returncode != 0 or not os.path.exists(prev):
            failures.append(f"preview({tag}): render failed")
            return
        logs = (r1.stdout or "") + (r1.stderr or "")
        if "VF_TEST_BRUSH" not in logs:
            failures.append(f"preview({tag}): brush hook never ran")
        _, _, b = read_ppm(prev)
        d, wr, wb, wg = diff_stats(w, h, a, w, h, b)
        print(f"[preview] {tag} tint on hover: pixel diff {d*100:.2f}%  "
              f"brighter R/G/B {wr}/{wg}/{wb}")
        if d < 0.0005:
            failures.append(f"preview({tag}): no visible highlight ({d*100:.3f}%)")
        if d > 0.25:
            failures.append(f"preview({tag}): highlight covers too much ({d*100:.1f}%)")
        if warm and not (wr > wb and wr > 0):
            failures.append(f"preview({tag}): tinted pixels are not warm")
        if green and not (wg > wr and wg > wb):
            failures.append(f"preview({tag}): tinted pixels are not green")
        if blue and not (wb > wr and wb > wg):
            failures.append(f"preview({tag}): tinted pixels are not blue")

    preview("carve", {"VF_TEST_BRUSH": f"{CELL},carve", "VF_EDIT_DIAM": "1.5"},
            warm=True)
    preview("add", {"VF_TEST_BRUSH": f"{CELL},add", "VF_EDIT_DIAM": "1.5",
                    "VF_EDIT_DEPTH": "1.0"}, green=True)
    preview("smooth", {"VF_TEST_BRUSH": f"{CELL},smooth", "VF_EDIT_DIAM": "1.5",
                       "VF_SMOOTH_STRENGTH": "0.8"}, blue=True)
    check_depth_sensitivity(binary, tmp, failures)


# Noise floor for the depth A/B, measured with diff_stats at its default
# thresh=10 on these 480x270 preview frames (two renders of an IDENTICAL
# --shot config, TAA jitter): 0.008%. The same pair at thresh=0 differs by
# 2.6%, which is the number to NOT use - the per-channel threshold is what
# removes the jitter, so comparing a thresh=10 signal against an unfiltered
# noise floor overstates the noise by ~300x and rejects real signal.
# 0.5% leaves a 60x margin over the measured floor and ~3.5x under the
# observed depth signal (1.74% for Carve 0.5 -> 6.0 m).
NOISE_FLOOR = 0.005


def check_depth_sensitivity(binary, tmp, failures):
    """The Depth slider must change the frame.

    Regression gate. The preview tint can only mark EXISTING surfels, and the
    extra depth of a Carve cylinder lies BELOW the surface (solid material)
    while an Add dome grows into empty air - neither has a surfel to tint. So
    the tint alone could never show depth, and measured on this exact cell the
    Carve affected-surfel mask was bit-identical (2920 px) at depths
    0.1/0.5/2.0/12.0 m: the slider moved nothing at all.

    Depth now has its own screen-space marker in the post pass (a line from the
    hit to the volume's far end plus a tick there). Two things the first version
    got wrong, both caught by this gate:
      - drawn as a wide soft gradient, which moves most pixels by only 1-10
        codes and is therefore invisible to diff_stats at its default thresh=10;
      - unclamped, a deep brush's far end projects metres OUTSIDE the frustum
        (it lies below the surface, and the camera looks along a shallow angle),
        so the line left the frame within a few pixels and every depth drew the
        same stub - 0.67% between 0.5 and 6.0 m.
    Now an opaque 1.5 px core with a 5 px halo and both ends clamped into the
    viewport: 1.74% for Carve 0.5 -> 6.0 m against a 0.008% noise floor.
    """
    def depth_pair(tag, mode, d_lo, d_hi):
        lo = os.path.join(tmp, f"depth_{tag}_lo.ppm")
        hi = os.path.join(tmp, f"depth_{tag}_hi.ppm")
        base = {"VF_TEST_BRUSH": f"{CELL},{mode}", "VF_EDIT_DIAM": "1.5"}
        for out, dep in ((lo, d_lo), (hi, d_hi)):
            env = dict(base, VF_EDIT_DEPTH=str(dep))
            r = render(binary, out, env, mode=None)
            if r.returncode != 0 or not os.path.exists(out):
                failures.append(f"depth({tag}): render failed at depth {dep}")
                return
        w, h, a = read_ppm(lo)
        _, _, b = read_ppm(hi)
        d = diff_stats(w, h, a, w, h, b)[0]
        print(f"[depth] {tag} {d_lo} m vs {d_hi} m: pixel diff {d*100:.2f}%")
        if d < NOISE_FLOOR:
            failures.append(
                f"depth({tag}): Depth slider is invisible ({d*100:.2f}% <= "
                f"{NOISE_FLOOR*100:.0f}% noise floor) - the depth marker is "
                f"missing or occluded")

    depth_pair("carve", "carve", 0.5, 6.0)
    depth_pair("add", "add", 0.5, 6.0)
    # The large-depth pair is the anti-saturation case. A screen-space line is
    # bounded by the frame, so once the volume's far end leaves the viewport the
    # clamped line used to pin to an edge and 6 m looked exactly like 12 m. The
    # cut-off cap plus the moving clamped end keep them apart (measured 3.0%).
    depth_pair("carve_deep", "carve", 6.0, 12.0)
    check_svo_preview(binary, tmp, failures)


def check_svo_preview(binary, tmp, failures):
    """The SVO reference backend must show the brush preview too.

    Regression gate. setBrush fed only SplatPass, so in --mode svo the brush
    had no preview whatsoever and every size/depth change was invisible. The
    BrushUBO is now bound at 13 in the SVO pipeline with the same std140
    layout, tested against the raymarch hit point.
    """
    base = os.path.join(tmp, "svo_preview_base.ppm")
    prev = os.path.join(tmp, "svo_preview_on.ppm")
    r0 = render(binary, base, {"VF_NO_OVERLAY": "1"}, mode="svo")
    if r0.returncode != 0 or not os.path.exists(base):
        failures.append("svo preview: baseline render failed")
        return
    r1 = render(binary, prev,
                {"VF_TEST_BRUSH": f"{CELL},delete", "VF_EDIT_DIAM": "6.0"},
                mode="svo")
    if r1.returncode != 0 or not os.path.exists(prev):
        failures.append("svo preview: preview render failed")
        return
    w, h, a = read_ppm(base)
    _, _, b = read_ppm(prev)
    d, wr, wb, wg = diff_stats(w, h, a, w, h, b)
    print(f"[svo preview] tint on hover: pixel diff {d*100:.2f}%  "
          f"brighter R/G/B {wr}/{wg}/{wb}")
    if d < 0.01:
        failures.append(
            f"svo preview: SVO backend shows no brush preview ({d*100:.2f}%)")
    # Delete tints red, so the changed pixels must be red-dominant - that also
    # proves it is the TINT and not a broken frame or a lighting change.
    if not (wr > wb and wr > 0):
        failures.append("svo preview: SVO preview is not red (Delete tint)")


def region_mean(img, w, x0, y0, x1, y1):
    s = n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            k = y * w + x
            s += (img[3 * k] + img[3 * k + 1] + img[3 * k + 2]) / 3.0
            n += 1
    return s / n


def check_undo_and_clear(binary, tmp, failures, baseline):
    """Undo and "Clear live edits" must actually remove live geometry.

    Regression: a chunk whose run became empty was never patched (an empty
    vector's data() is null, which tripped patchChunkSurfels' null-data
    guard), so an undone stroke - and a cleared world - kept rendering the
    removed material. Overlay reads/writes go through VF_OVERLAY_PATH into the
    temp dir, so a session's own painting is never touched.

    The raise stroke at CELL fills the frame centre at this close-up camera
    (measured there: mean luma ~51 vs ~92 untouched); both reverts must bring
    that region back to the untouched luma (the touched chunks' store-derived
    shading can shift it a few counts until the next full reload).
    """
    over = os.path.join(tmp, "runtime_edits.vxw")
    env = {"VF_OVERLAY_PATH": over}

    def blob(img):
        return region_mean(img, W, 150, 90, 460, 250)

    w, h, base = read_ppm(baseline)
    b0 = blob(base)
    stroke = {"VF_TEST_STROKE": f"{CELL},3,raise", "VF_TEST_STROKE_SAVE": "1"}

    # 1) a stroke + save (mirrors the interactive release path)
    sa = os.path.join(tmp, "undo_stroke.ppm")
    r = render(binary, sa, dict(env, **stroke), mode=None)
    if r.returncode != 0 or not os.path.exists(sa):
        failures.append("undo: stroke render failed")
        return
    if not os.path.exists(over):
        failures.append("undo: the stroke did not persist an overlay")
    _, _, ai = read_ppm(sa)
    print(f"[undo] blob luma untouched {b0:.0f}, stroke {blob(ai):.0f}")
    if blob(ai) > b0 - 20:
        failures.append(f"undo: the stroke is not visible (blob {blob(ai):.0f})")

    # 2) undo: the geometry must be gone again (emptied chunks included)
    un = os.path.join(tmp, "undo_undone.ppm")
    r = render(binary, un,
               dict(stroke, VF_OVERLAY_PATH=os.path.join(tmp, "undo_overlay.vxw"),
                    VF_TEST_UNDO="1"), mode=None)
    logs = (r.stdout or "") + (r.stderr or "")
    if r.returncode != 0 or not os.path.exists(un):
        failures.append("undo: undo render failed")
        return
    for key in ("live edit undo:", "undo: stroke recorded"):
        if key not in logs:
            failures.append(f"undo: log missing '{key}'")
    _, _, ui = read_ppm(un)
    print(f"[undo] undone blob luma {blob(ui):.0f}")
    if blob(ui) < blob(ai) + 20:
        failures.append("undo: the undone stroke still renders")
    if abs(blob(ui) - b0) > 12:
        failures.append("undo: the undone frame does not match the untouched ground")

    # 3) restore the saved overlay, then Clear live edits
    cl = os.path.join(tmp, "undo_cleared.ppm")
    r = render(binary, cl, dict(env, VF_TEST_CLEAR="1"), mode=None, overlay=True)
    logs = (r.stdout or "") + (r.stderr or "")
    if r.returncode != 0 or not os.path.exists(cl):
        failures.append("clear: render failed")
        return
    if "live overlay: restored" not in logs:
        failures.append("clear: the saved overlay was not restored (test premise)")
    if "live edits: cleared" not in logs:
        failures.append("clear: never logged 'live edits: cleared'")
    if os.path.exists(over):
        failures.append("clear: the overlay file survived the clear")
    _, _, ci = read_ppm(cl)
    print(f"[clear] cleared blob luma {blob(ci):.0f} (stroke {blob(ai):.0f})")
    if blob(ci) < blob(ai) + 20:
        failures.append("clear: the cleared geometry still renders")


def render_batch(binary, tmp, views, mode=None, extra_env=None):
    """One process renders every untouched baseline of a (mode, env) group: a
    world load is ~13 s and dominates a single --shot, so the suite batches.
    views: list of (out basename, cam)."""
    list_path = os.path.join(tmp, "shots.txt")
    with open(list_path, "w") as lf:
        for name, cam in views:
            lf.write(" ".join([os.path.join(tmp, name + ".ppm"), *cam]) + "\n")
    env = dict(os.environ, VF_NO_OVERLAY="1")
    if extra_env:
        env.update(extra_env)
    cmd = [binary, "--shotlist", list_path, "--width", str(W), "--height", str(H)]
    if mode:
        cmd += ["--mode", mode]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=900, env=env)
    if r.returncode != 0:
        print(r.stderr[-2000:])
    return r.returncode == 0


def check_pair(binary, tmp, tag, mode, failures, min_diff=0.02, max_diff=0.60,
               max_black=0.05, edit=EDIT, base_name=None, extra_env=None,
               cam=None):
    logs = ""
    # the untouched frame is shared between all checks on the same backend
    base = os.path.join(tmp, base_name or f"{tag}_base.ppm")
    editp = os.path.join(tmp, f"{tag}_edit.ppm")
    if os.path.exists(base):
        r0 = None
    else:
        r0 = render(binary, base, extra_env, mode=mode, cam=cam)
    if r0 is not None and (r0.returncode != 0 or not os.path.exists(base)):
        failures.append(f"{tag}: baseline render failed")
        return logs
    env = dict(extra_env) if extra_env else {}
    env["VF_TEST_EDIT"] = edit
    r1 = render(binary, editp, env, mode=mode, cam=cam)
    if r1.returncode != 0 or not os.path.exists(editp):
        failures.append(f"{tag}: edited render failed")
        return logs
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
    return logs


def main():
    # `--only <name>` runs a single self-contained check group and renders its
    # own baselines, so a preview-only change is verifiable without paying for
    # the whole live-edit matrix (whose wall time is dominated by per-run world
    # loads). `preview` is the selector behind the `test-preview` group.
    only = None
    args = sys.argv[1:]
    if "--only" in args:
        i = args.index("--only")
        if i + 1 >= len(args):
            print("--only needs a group name")
            return 2
        only = args[i + 1]
        args = args[:i] + args[i + 2:]
    if len(args) != 1:
        print(__doc__)
        return 2
    binary = os.path.abspath(args[0])
    failures = []

    if only is not None:
        if only != "preview":
            print(f"unknown --only group {only!r} (known: preview)")
            return 2
        with tempfile.TemporaryDirectory() as tmp:
            check_preview(binary, tmp, failures,
                          os.path.join(tmp, "splat_base.ppm"))
        if failures:
            for f in failures:
                print("FAIL:", f)
            return 1
        print("preview_check PASSED")
        return 0

    if FAST:
        # Fast iteration profile: retain the core live-store patch assertion,
        # but defer the exhaustive backend/water/undo matrix to the full gate.
        with tempfile.TemporaryDirectory() as tmp:
            render_batch(binary, tmp, [("splat_base", CAM)])
            check_pair(binary, tmp, "splat_fast", None, failures,
                       min_diff=0.05, max_diff=0.70, max_black=0.09,
                       base_name="splat_base.ppm")
        if failures:
            for f in failures:
                print("FAIL:", f)
            return 1
        print("live_edit_check FAST PASSED")
        return 0

    with tempfile.TemporaryDirectory() as tmp:
        # every untouched baseline first, batched per (mode, env): the checks
        # below find the files present and skip their own render
        render_batch(binary, tmp, [
            ("splat_base", CAM),
            ("hero_base", HERO_CAM),
            ("water_base", WATER_CAM),
            ("water_channel_base", CHANNEL_CAM),
        ])
        render_batch(binary, tmp, [("splat_nomicro_base", CAM)],
                     extra_env={"VF_MICRO": "0"})
        render_batch(binary, tmp, [("svo_base", CAM)], mode="svo")
        # splat (default) and the SVO reference both patch the same edit
        splat_log = check_pair(binary, tmp, "splat", None, failures, min_diff=0.05,
                               max_diff=0.70, max_black=0.09,
                               base_name="splat_base.ppm")
        check_pair(binary, tmp, "svo", "svo", failures, min_diff=0.03, max_diff=0.70,
                   max_black=0.09, base_name="svo_base.ppm")
        # store-only brush modes (no record-layer equivalent): delete clears the
        # brush ball, paint recolours it. Micro-detail off so the diff is the
        # edit geometry itself instead of micro-disk noise (the patched chunk
        # keeps its micro tail now - see check_micro_persistence).
        no_micro = {"VF_MICRO": "0"}
        check_pair(binary, tmp, "delete", None, failures, min_diff=0.02, max_diff=0.70,
                   max_black=0.09, edit=CELL + ",delete",
                   base_name="splat_nomicro_base.ppm", extra_env=no_micro)
        check_pair(binary, tmp, "paint", None, failures, min_diff=0.02, max_diff=0.70,
                   max_black=0.09, edit=CELL + ",paint",
                   base_name="splat_nomicro_base.ppm", extra_env=no_micro)
        # object-surface smooth: a picked object cell must relax the surface
        # along its own axis (never the terrain column path) and patch the
        # splats over the exact store band.
        obj_logs = check_pair(binary, tmp, "object_smooth", None, failures,
                              min_diff=0.01, max_diff=0.50, max_black=0.09,
                              edit=OBJECT_CELL + ",smooth",
                              base_name="hero_base.ppm",
                              extra_env={"VF_EDIT_DIAM": "4.0", "VF_TRACE": "1"},
                              cam=HERO_CAM)
        if "smooth object:" not in obj_logs:
            failures.append("object_smooth: object pick did not run the "
                            "surface-axis relaxation")
        if "smooth terrain:" in obj_logs:
            failures.append("object_smooth: object pick ran the terrain path")
        # a live-patched chunk must keep its deterministic micro-detail tail
        # (reuses the splat edit run's log instead of re-rendering it)
        check_micro_persistence(binary, tmp, failures, splat_log)
        # carve/add/smooth hover previews (tint only, no edit)
        check_preview(binary, tmp, failures,
                      os.path.join(tmp, "splat_base.ppm"))
        check_per_voxel(binary, tmp, failures)
        check_object_undo_surgical(binary, tmp, failures)
        # a dug volume below the water level reads as the fixed-level plane
        check_water_fill(binary, tmp, failures)
        # undo + "Clear live edits" must remove live geometry (and the overlay)
        check_undo_and_clear(binary, tmp, failures,
                             os.path.join(tmp, "splat_base.ppm"))

    if failures:
        for f in failures:
            print("FAIL:", f)
        return 1
    print("live_edit_check PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
