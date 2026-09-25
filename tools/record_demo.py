#!/usr/bin/env python3
"""record_demo.py - produce a demo reel of the default lakeside hamlet.

Two modes, because they solve different problems:

  --mode shots   (default, RECOMMENDED)
      Renders a keyframed tour of the hamlet through `--shotlist`. Each
      "variant" (backend, splat radius, micro/LOD) is one app launch and
      therefore one world load, but all keyframes inside a variant share
      that single load. Deterministic: same input, same frames. Use this for
      anything you want to compare or publish. `--sheet` builds a per-shot
      contact sheet so voxel<->splat and radius changes sit side by side.

  --mode live
      Drives the real GLFW window with XTEST input (tools/vf_input.py) and
      screen-captures it, so the sidebar / HUD / brush tints are visible.
      This is the only mode that can show UI, but it needs an idle display
      and takes a world load per run. Refuses to start while another
      Voxelforge window is open.

  --mode dual
      Records ONE scripted tour (tools/vf_tour.py) twice -- once with the
      splat backend, once with --mode svo -- and concatenates the clips into
      a single file. Both passes replay the identical input from the same
      spawn pose, so the only difference between the halves is the renderer.
      `--seconds` sets the per-pass length; two passes make the final video
      twice that long.

Examples:
    python3 tools/record_demo.py --list                 # keyframes + variants
    python3 tools/record_demo.py --out /tmp/opencode/demo --sheet
    python3 tools/record_demo.py --variants splats voxel   # just the A/B
    python3 tools/record_demo.py --mode dual --seconds 60  # 120s A/B video
    python3 tools/record_demo.py --mode live --out /tmp/opencode/live
"""

import argparse
import os
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "voxelforge")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

TITLE = "Voxelforge"

# Keyframed tour of the default lakeside hamlet. Each row is
#   name  camx camy camz  targetx targety targetz
# The camera is the first triple, the target the second; the app builds the
# view basis from them, so these are eye->look-at pairs.
#
# Ground truth for these numbers (measured, not guessed):
#   * world coords: voxel = (cell - 512) * VOXEL, VOXEL = 0.1
#   * bounds from `tools/vxw_dump.py --layer <name>`; CENTRES below are the
#     layers that are actually ENABLED in assets/world.json
#       CabinPart1      x[544..592] y[510..538] z[599..649] -> c( 5.6, 1.2, 11.2)  4.9x2.9x5.1m
#       hamlet_tower    x[687..735] y[516..643] z[667..715] -> c(19.9, 6.7, 17.9)  12.8m TALL
#       hamlet_boat     x[644..662] y[497..548] z[526..586] -> c(14.1, 1.0,  4.4)
#       hamlet_garden   x[525..620] y[510..546] z[666..769] -> c( 6.0, 1.6, 20.5)
#       hamlet_trees    x[663..832] y[521..675] z[643..790] -> c(22.3, 9.5, 21.6)  17x15x15m
#       hamlet_reeds    x[468..794] y[499..521] z[473..560] -> c(-0.3, 0.8,  4.3)  32.7m wide
#       hamlet_reeds_far x[468..791] y[496..521] z[562..639]-> c(-0.1, 0.8,  9.3)
#   * DISABLED in the shipped manifest -- do NOT frame these, they are not
#     drawn: hamlet_hall, hamlet_pier, hamlet_well, hamlet_market,
#     hamlet_props (all `enabled: false`). An earlier version of this list
#     aimed three keyframes at them and framed empty ground.
#   * filter layers by `role == "object" and enabled`; `role` is "object" or
#     "landscape", which is stable across re-authors (filenames are not).
#   * RE-DERIVE, DON'T TRUST: these entries are hard-coded world COORDINATES
#     measured from the bounds above. Nothing here reads a layer by name, so
#     after a re-author this script will NOT error -- it will silently frame
#     whatever now occupies those coordinates, or empty ground. That is the
#     same failure class as the disabled-layer bug fixed above. To make it
#     self-updating instead, resolve subjects from world.json by
#     role/enabled the way `visual_check.py:ownership_layer()` does, and
#     compute the camera from the chosen layer's bounds.
#   * terrain height from assets/heightmap.png (meters map [-8, 24]):
#       hall 10.9m, tower 2.4m, pier 19.7m, well -0.6m, market -3.3m, garden -5.6m
#   * water plane is y = -0.9, so anything under that reads as water
#
# Consequences baked into the cameras below: the terrain SWEEPS from ~20m at
# the pier end down below the waterline at the market/garden end, so camera
# heights are per-shot, not a constant.
SHOTS = [
    # --- village core -------------------------------------------------
    ("01_village_hero",  0.0, 7.0,  0.0,   15.0, 3.0, 15.0),
    ("02_cabin_front",   1.0, 3.2, 16.5,    5.8, 1.3, 11.2),
    ("03_tower_low",    12.0, 3.0, 23.0,   19.9, 7.5, 17.9),   # look UP at it
    ("04_tree_grove",    8.0, 5.0, 12.0,   22.3, 6.0, 21.6),
    ("05_garden",        2.0, 3.0, 24.0,    7.0, 1.5, 20.5),
    # --- water --------------------------------------------------------
    ("06_reeds_low",     6.0, 1.6, -1.0,   -0.3, 1.0,  5.0),   # low over the reeds
    ("07_boat_close",   16.5, 2.0,  2.0,   14.1, 1.2,  4.4),
    ("08_shore_grass",   6.0, 1.5,  2.0,   12.0, 1.0, 12.0),   # low over water
    # --- scale --------------------------------------------------------
    ("09_overview_high", 4.0, 18.0, -2.0,  16.0, 2.0, 15.0),
    ("10_village_wide", 26.0, 9.0,  4.0,   14.0, 3.0, 15.0),   # from the east
]


# Variants. `--shotlist` renders every row under ONE world load, but the
# backend and the splat knobs are per-invocation (env / --mode), so comparing
# them costs one app launch each. `splats` is the reference pass; the rest are
# labelled A/B views of the same cameras.
#
# The env values are real and read at runtime:
#   VF_SPLAT_RADIUS  direct disk-scale multiplier, clamped 0.5..2.0 in
#                    SplatPass::record (the same knob the sidebar slider and
#                    the [ / ] keys drive)
#   VF_MICRO=0       micro-surfel detail off (3.40M -> 2.13M surfels)
#   VF_LOD=0         no merged-terrain LOD rings
#   --mode svo       the chunked-SVO voxel raymarcher, the pixel reference for
#                    the primary splat backend
VARIANTS = [
    ("splats",     [],                          "splat"),
    ("voxel",      [],                          "svo"),
    ("radius_min", ["VF_SPLAT_RADIUS=0.5"],     "splat"),
    ("radius_max", ["VF_SPLAT_RADIUS=2.0"],     "splat"),
    ("no_micro",   ["VF_MICRO=0", "VF_LOD=0"], "splat"),
]


def build_shotlist(path, width, height):
    """Write one keyframe list. Returns the number of shots written."""
    with open(path, "w") as f:
        f.write("# path camx camy camz tx ty tz\n")
        f.write(f"# generated by record_demo.py  {width}x{height}\n")
        for name, cx, cy, cz, tx, ty, tz in SHOTS:
            out = os.path.splitext(path)[0] + f"_{name}.ppm"
            f.write(f"{out} {cx} {cy} {cz} {tx} {ty} {tz}\n")
    return len(SHOTS)


def run_variant(args, outdir, tag, envset, backend):
    """Render every SHOT under one (env, backend) pair -- one world load."""
    vdir = os.path.join(outdir, tag)
    os.makedirs(vdir, exist_ok=True)
    listfile = os.path.join(vdir, "shots.txt")
    n = build_shotlist(listfile, args.width, args.height)

    env = dict(os.environ)
    env["VF_NO_OVERLAY"] = "1"      # don't record someone's paint session
    env["VF_OVERLAY_PATH"] = os.path.join(outdir, "overlay.vxw")
    for kv in envset:
        k, _, val = kv.partition("=")
        env[k] = val

    cmd = [BIN, "--shotlist", listfile,
           "--width", str(args.width), "--height", str(args.height),
           "--mode", backend]
    if args.animtime:
        cmd += ["--animtime", str(args.animtime)]
    if args.sun:
        cmd += ["--sun", str(args.sun[0]), str(args.sun[1])]
    label = " ".join(envset) or "(defaults)"
    print(f"[{tag}] {n} shots, mode={backend}, env={label}")
    t0 = time.time()
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"[{tag}] FAILED rc={r.returncode}", file=sys.stderr)
        print("\n".join(r.stderr.splitlines()[-8:]), file=sys.stderr)
        return []
    print(f"[{tag}] done in {time.time() - t0:.1f}s (one world load)")
    return sorted(f for f in os.listdir(vdir) if f.endswith(".ppm"))


def run_shots(args, outdir):
    os.makedirs(outdir, exist_ok=True)
    wanted = ([v for v in VARIANTS if v[0] in args.variants]
              if args.variants else VARIANTS)

    all_frames = []
    for i, (tag, envset, backend) in enumerate(wanted, 1):
        print(f"\n=== variant {i}/{len(wanted)}: {tag} ===")
        all_frames += run_variant(args, outdir, tag, envset, backend)

    if not all_frames:
        print("ERROR: no frames produced", file=sys.stderr)
        return 1
    print(f"\n[shots] {len(all_frames)} frames total across "
          f"{len(wanted)} variants in {outdir}")

    # Per-keyframe acceptance. The hard-coded coordinates in SHOTS are the
    # QUIET failure mode: after a re-author a keyframe still renders, just of
    # empty ground, and nothing complains. This turns that into an error.
    if args.check:
        bad = 0
        for tag, _, _ in wanted:
            for s in SHOTS:
                name = s[0]
                p = os.path.join(outdir, tag, "shots_%s.ppm" % name)
                if not os.path.exists(p):
                    continue
                m = check_frame(p)
                if not m["ok"]:
                    print("[check] FAIL %s/%s: %s" % (tag, name, m["why"]),
                          file=sys.stderr)
                    bad += 1
                elif args.verbose:
                    print("[check] ok   %s/%-18s coverage %.3f  sky %.2f"
                          % (tag, name, m["coverage"], m["sky_probe"]))
        print("[check] %d frame(s) failed the acceptance band" % bad)
        if bad:
            print("[check] a failing keyframe usually means SHOTS points at "
                  "content that is no longer there (disabled layer, moved "
                  "object) - re-derive from tools/vxw_dump.py --layer <name>.",
                  file=sys.stderr)
            if not args.lenient:
                return 1

    # A/B contact sheet per camera: the same shot across every variant, so the
    # voxel<->splat and radius changes are directly comparable.
    if len(wanted) > 1 and args.sheet:
        build_sheets(outdir, wanted, args)
    if args.ffmpeg:
        encode(outdir, all_frames, args)
    return 0


def build_sheets(outdir, wanted, args):
    """One contact sheet per SHOT: rows = variants, columns = fixed width."""
    try:
        import numpy as np
        from PIL import Image, ImageDraw
    except Exception as e:
        print(f"[sheet] skipped ({e})")
        return
    sw, sh = 320, 180
    for _, name, *_ in [(s[0], s[0]) for s in SHOTS]:
        imgs = []
        for tag, _, _ in wanted:
            p = os.path.join(outdir, tag, f"shots_{name}.ppm")
            if not os.path.exists(p):
                p = os.path.join(outdir, tag, f"*{name}.ppm")
            try:
                imgs.append((tag, Image.open(p).convert("RGB").resize((sw, sh))))
            except Exception:
                imgs.append((tag, Image.new("RGB", (sw, sh), (20, 20, 20))))
        if not imgs:
            continue
        pad = 18
        sheet = Image.new("RGB", (sw * len(imgs), sh + pad), (12, 12, 12))
        d = ImageDraw.Draw(sheet)
        for i, (tag, im) in enumerate(imgs):
            sheet.paste(im, (i * sw, pad))
            d.text((i * sw + 6, 4), tag, fill=(235, 235, 235))
        out = os.path.join(outdir, f"sheet_{name}.png")
        sheet.save(out)
    print(f"[sheet] {len(SHOTS)} comparison sheets in {outdir}")


def encode(outdir, frames, args):
    """Stitch PPM frames to mp4 with ffmpeg (optional, cosmetic)."""
    out = os.path.join(outdir, "demo.mp4")
    listfile = os.path.join(outdir, "frames.txt")
    with open(listfile, "w") as f:
        for fr in frames:
            f.write(f"file '{os.path.join(outdir, fr)}'\nduration 2\n")
        f.write(f"file '{os.path.join(outdir, frames[-1])}'\n")
    cmd = ["ffmpeg", "-y", "-loglevel", "error", "-f", "concat",
           "-safe", "0", "-i", listfile,
           "-vf", f"scale={args.width}:{args.height},format=yuv420p",
           "-r", str(args.fps), out]
    print("[shots] encoding", out)
    subprocess.run(cmd, check=True)


# ------------------------------------------------------------------ live

def find_windows(root, title):
    hits = []
    stack = [root]
    while stack:
        w = stack.pop()
        try:
            if w.get_wm_name() == title:
                g = w.get_geometry()
                tr = root.translate_coords(w, 0, 0)
                hits.append((w, (tr.x, tr.y, g.width, g.height)))
            stack.extend(w.query_tree().children)
        except Exception:
            pass
    return hits


def _mean_luma(png):
    """Mean luma of a PNG, or None if it cannot be read."""
    try:
        import numpy as np
        from PIL import Image
        a = np.asarray(Image.open(png).convert("RGB"), dtype=np.float32)
        return float(a.mean())
    except Exception:
        return None


def top_window_at(root, px, py):
    """Top-most window containing (px, py), deepest-wins. Or None.

    Walks the stacking order front-to-back and returns the first window whose
    root-space rect contains the point. Used to PROVE what an x11grab of that
    rect will actually sample, instead of inferring it from pixel statistics.
    """
    def inside(w, x, y):
        try:
            t = root.translate_coords(w, 0, 0)
            g = w.get_geometry()
            return (t.x <= x < t.x + g.width and t.y <= y < t.y + g.height)
        except Exception:
            return False

    def rec(w):
        if inside(w, px, py):
            try:
                return w
            except Exception:
                return None
        try:
            kids = [c for c in w.query_tree().children if inside(c, px, py)]
        except Exception:
            return None
        for c in reversed(kids):
            hit = rec(c)
            if hit is not None:
                return hit
        return None

    try:
        stack = list(root.query_tree().children)
    except Exception:
        return None
    for w in reversed(stack):
        hit = rec(w)
        if hit is not None:
            return hit
    return None


def describe_window(root, w):
    """(id, wm_name, root_x, root_y, width, height, map_state) for logging.

    Takes `root` explicitly: `w.display` is a _BaseDisplay with no .screen(),
    so deriving the root from the window raises and -- because this runs on
    BOTH sides of the identity comparison -- makes the check fail closed on
    its own bug rather than on a real mismatch.
    """
    try:
        g = w.get_geometry()
        t = root.translate_coords(w, 0, 0)
        return (hex(w.id), w.get_wm_name(), t.x, t.y, g.width, g.height,
                w.get_attributes().map_state)
    except Exception as e:
        return ("?", "error:%s" % e, 0, 0, 0, 0, -1)


def verify_capture_target(root, app_win, geom, sw=None, sh=None):
    """Assert the grab rect really samples `app_win`. Returns (ok, detail).

    A windowed capture that cannot prove what it sampled must not be
    trusted: an x11grab reads the composited display, so an overlapping
    window silently yields a well-formed image of the WRONG surface. Pixel
    statistics cannot detect that, so check identity directly.

    sw/sh are the SCREEN size in pixels, passed in deliberately: a Window has
    no .screen(), and deriving it here raised an exception that a bare
    `except: pass` swallowed -- so the WM-frame exemption below silently
    never ran and the check failed on its own bug.
    """
    x, y, w, h = geom
    cx, cy = x + w // 2, y + h // 2
    top = top_window_at(root, cx, cy)
    detail = {"app": describe_window(root, app_win), "geom": geom}
    if top is None:
        detail["top"] = None
        return False, detail
    detail["top"] = describe_window(root, top)
    if top.id == app_win.id:
        return True, detail
    # A parent/child relationship still means we sampled the app's surface.
    try:
        p = top.parent()
        if p and p.id == app_win.id:
            detail["note"] = "top is a child of the app window"
            return True, detail
    except Exception:
        pass
    # Cinnamon/Muffin keep UNNAMED windows stacked above the client: the WM
    # decoration FRAME that surrounds the app (a few px larger than the app
    # rect), and full-screen overlay/guard windows. Both are normally
    # transparent, so their presence in the stacking order says nothing about
    # what the grab samples -- and treating them as obstructions makes this
    # check fail on every run. Only a NAMED window that is not ours counts.
    try:
        tg = top.get_geometry()
        tt = root.translate_coords(top, 0, 0)
        tname = top.get_wm_name()
        fullscreen = bool(sw and sh and tg.width >= sw * 0.95
                          and tg.height >= sh * 0.95)
        contains = (tt.x <= x and tt.y <= y
                    and tt.x + tg.width >= x + w and tt.y + tg.height >= y + h)
        if not tname and (fullscreen or contains):
            detail["note"] = ("top is an unnamed WM window (%dx%d at %d,%d) "
                              "framing/full-screen over the app - normally "
                              "transparent, proceeding"
                              % (tg.width, tg.height, tt.x, tt.y))
            return True, detail
    except Exception as e:
        # Do NOT swallow this quietly: a silent except here previously hid a
        # bad attribute access and made the whole check fail on its own bug.
        detail["note"] = "WM-frame exemption error: %r" % (e,)
        return False, detail
    return False, detail


def vram_report():
    """(used_MiB, total_MiB, [foreign processes]) from nvidia-smi, or None.

    A windowed capture can go dark and input-dead purely because VRAM is
    exhausted -- measured: an offscreen --shot of the same camera stayed
    correct (luma 123.6) while the live window dropped to 29.1 and stopped
    receiving keys, with two llama-server processes holding 8.8 GB. Check
    this before blaming a shader or the capture code.
    """
    try:
        r = subprocess.run(
            ["nvidia-smi", "--query-gpu=memory.used,memory.total",
             "--format=csv,noheader,nounits"],
            capture_output=True, text=True, timeout=15)
        used, total = (int(x) for x in r.stdout.strip().splitlines()[0].split(","))
        apps = subprocess.run(
            ["nvidia-smi", "--query-compute-apps=pid,used_memory",
             "--format=csv,noheader,nounits"],
            capture_output=True, text=True, timeout=15).stdout.strip().splitlines()
        return used, total, apps
    except Exception:
        return None


# Frame acceptance band, quoted from tests/visual_check.py:analyze() so the
# two cannot drift apart silently. That file is the authority -- if you change
# a number here, change it there.
#   sky pixel   : b > r+12 and g > r+4 and b > 120
#   coverage    : fraction of NON-sky pixels, must satisfy 0.03 <= c <= 0.985
#   sky probe   : top 1/8 strip, fraction with b >= r, must be > 0.5
# The UPPER bound is 0.985 rather than something stricter on purpose: a close
# water/boat frame legitimately fills the lens, and that is still healthy. A
# floor-only test would pass both an empty field and a lens-filling wall, so
# the band plus the sky probe is the assertion, not the floor.
COV_MIN, COV_MAX = 0.03, 0.985


def check_frame(path):
    """Apply visual_check.py's acceptance rule to a PPM. Returns a dict."""
    try:
        import numpy as np
        from PIL import Image
        a = np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)
    except Exception as e:
        return {"ok": False, "why": "unreadable: %s" % e}
    r, g, b = a[:, :, 0], a[:, :, 1], a[:, :, 2]
    is_sky = (b > r + 12) & (g > r + 4) & (b > 120)
    cov = float((~is_sky).mean())
    h = a.shape[0]
    top = a[: max(1, h // 8)]
    probe = float((top[:, :, 2] >= top[:, :, 0]).mean())
    why = None
    if not (COV_MIN <= cov <= COV_MAX):
        why = ("coverage %.3f out of range [%.2f, %.3f] -- too little world "
               "(empty/swallowed frame) or the lens is filled (buried camera)"
               % (cov, COV_MIN, COV_MAX))
    elif probe <= 0.5:
        why = "sky probe %.2f not blue-dominant (top strip)" % probe
    return {"ok": why is None, "coverage": cov, "sky_probe": probe,
            "why": why}


def wait_for_clear(root, title, samples=5, gap=2.0, timeout=180.0):
    """Block until no live window with `title` exists for `samples` in a row.

    A single clear sample is not enough: drivers like live_edit_check.py
    render sequentially, so the display is briefly empty between shots and a
    one-shot check races straight into the next render.
    """
    """Block until no live window with `title` exists for `samples` in a row.

    A single clear sample is not enough: drivers like live_edit_check.py
    render sequentially, so the display is briefly empty between shots and a
    one-shot check races straight into the next render.
    """
    import os
    t0 = time.time()
    streak = 0
    while time.time() - t0 < timeout:
        live = []
        stack = [root]
        while stack:
            w = stack.pop()
            try:
                if w.get_wm_name() == title:
                    alive = True
                    try:
                        p = w.get_full_property(
                            root.display.intern_atom("_NET_WM_PID"),
                            0)  # X.AnyPropertyType
                        if p:
                            try:
                                os.kill(p.value[0], 0)
                            except OSError:
                                alive = False
                    except Exception:
                        pass
                    if alive:
                        g = w.get_geometry()
                        tr = root.translate_coords(w, 0, 0)
                        live.append((w, (tr.x, tr.y, g.width, g.height)))
                stack.extend(w.query_tree().children)
            except Exception:
                pass
        if not live:
            streak += 1
            if streak >= samples:
                return True, []
        else:
            streak = 0
        time.sleep(gap)
    return False, live


def run_live(args, outdir):
    from vf_input import VFInput

    os.makedirs(outdir, exist_ok=True)

    from Xlib import display
    d = display.Display(":0")
    root = d.screen().root

    # Guard on live windows, not processes: --shot/--shotlist own a titled
    # window even though they render offscreen, so only a window-based guard
    # is meaningful.
    clear, live = wait_for_clear(root, TITLE)
    if not clear:
        print("[live] REFUSING: a live Voxelforge window is still open. "
              "These share the WM_NAME and break capture.", file=sys.stderr)
        for w, g in live:
            print(f"   {hex(w.id)} {g}", file=sys.stderr)
        print("If a test driver is running, wait for it to finish.",
              file=sys.stderr)
        return 2
    pre = set()

    env = dict(os.environ, VF_NO_OVERLAY="1",
               VF_OVERLAY_PATH=os.path.join(outdir, "overlay.vxw"))
    print("[live] launching app")
    app = subprocess.Popen([BIN, "--width", str(args.width),
                            "--height", str(args.height)],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, bufsize=1, env=env)
    log = []
    threading.Thread(target=lambda: [log.append(l.rstrip())
                                     for l in app.stdout], daemon=True).start()

    try:
        geom = None
        want = (args.width, args.height)
        t0 = time.time()
        while time.time() - t0 < 60:
            # Select by size, not DFS order: hits[0] routinely picks a
            # leftover window from an earlier run at a different size.
            cands = [h for h in find_windows(root, TITLE) if h[0].id not in pre]
            exact = [h for h in cands if (h[1][2], h[1][3]) == want]
            pick = exact or sorted(cands, key=lambda h: -h[1][2] * h[1][3])
            if pick and (pick[0][1][2] >= 640 and pick[0][1][3] >= 360):
                geom = pick[0][1]
                break
            time.sleep(0.5)
        if geom is None:
            print("[live] no suitable window appeared", file=sys.stderr)
            return 1
        print(f"[live] window {geom}")

        inp = VFInput(title=TITLE, exclude=pre)
        print("[live] waiting for the world to load (~17.6 s)...")
        t0 = time.time()
        while time.time() - t0 < 240:
            if any("layered_world: load" in l for l in log):
                break
            time.sleep(0.5)
        if not any("layered_world: load" in l for l in log):
            print("[live] world never loaded", file=sys.stderr)
            return 1
        print(f"[live] loaded after {time.time() - t0:.1f}s")

        # The "layered_world: load" line fires when the world data is ready,
        # which is still BEFORE the GPU has uploaded and drawn it. Starting
        # the recorder there yields ~2-3 s of black frames at the head of the
        # video. Gate on the rendered frame actually being bright and stable.
        x, y, w, h = geom
        probe = os.path.join(outdir, "_probe.png")
        prev = None
        streak = 0
        t0 = time.time()
        bright = False
        while time.time() - t0 < 180:
            time.sleep(0.6)
            subprocess.run(
                ["ffmpeg", "-y", "-loglevel", "error", "-f", "x11grab",
                 "-video_size", f"{w}x{h}", "-i", f":0+{x},{y}",
                 "-frames:v", "1", probe], check=True)
            m = _mean_luma(probe)
            if m is not None and m > 40:
                if prev is not None and abs(m - prev) < 0.5:
                    streak += 1
                    if streak >= 3:
                        bright = True
                        break
                else:
                    streak = 0
            prev = m
        print(f"[live] frame ready: {bright} "
              f"(luma {m if m is None else round(m, 1)}, "
              f"+{time.time() - t0:.1f}s)")
        if not bright:
            print("[live] WARNING: frame never stabilised; recording anyway",
                  file=sys.stderr)

        inp.focus()
        inp.center()
        time.sleep(0.5)

        raw = os.path.join(outdir, "live.mp4")
        rec = subprocess.Popen(
            ["ffmpeg", "-y", "-loglevel", "error", "-f", "x11grab",
             "-framerate", str(args.fps),
             "-video_size", f"{w}x{h}", "-i", f":0+{x},{y}",
             "-c:v", "libx264", "-pix_fmt", "yuv420p", "-preset", "ultrafast",
             raw], stderr=subprocess.PIPE)
        print(f"[live] recording -> {raw}")

        # Timeline: a short scripted tour. Movement is dt-integrated in the
        # app, so these durations are wall-clock and framerate-independent.
        script = [
            ("hold W (fly forward)", lambda: inp.hold("w", 2.5)),
            ("look right", lambda: (inp.look_begin(),
                                    inp.look(420, 0, 2.0), inp.look_end())),
            ("hold A (strafe)", lambda: inp.hold("a", 1.2)),
            ("look down", lambda: (inp.look_begin(),
                                   inp.look(0, 160, 1.2), inp.look_end())),
            ("open the Edit panel", lambda: inp.tap("c", settle=1.0)),
            ("hold S (back up)", lambda: inp.hold("s", 1.5)),
            ("close the panel", lambda: inp.tap("c", settle=0.8)),
        ]
        for label, fn in script:
            print(f"[live]   {label}")
            fn()
        time.sleep(1.0)

        rec.terminate()
        try:
            rec.wait(timeout=10)
        except subprocess.TimeoutExpired:
            rec.kill()
        print(f"[live] saved {raw}")
        return 0
    finally:
        app.terminate()
        try:
            out, _ = app.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            app.kill()
            out = ""
        tail = [l for l in out.splitlines() if "layered_world" in l
                or "surfels" in l or "Swapchain" in l]
        print("\n--- app log ---")
        print("\n".join(tail[-5:]))


def record_pass(args, outdir, tag, backend, envset, seconds):
    """Launch the app, wait for a stable frame, record `seconds`, close.

    Returns the path to the recorded mp4, or None on failure.
    """
    from vf_input import VFInput
    from vf_tour import play

    from Xlib import display
    d = display.Display(":0")
    root = d.screen().root

    clear, live = wait_for_clear(root, TITLE)
    if not clear:
        print("[%s] REFUSING: live Voxelforge window open" % tag,
              file=sys.stderr)
        for w, g in live:
            print("   %s %s" % (hex(w.id), g), file=sys.stderr)
        return None
    pre = set()

    env = dict(os.environ, VF_NO_OVERLAY="1")
    env["VF_OVERLAY_PATH"] = os.path.join(outdir, "overlay.vxw")
    for kv in envset:
        k, _, val = kv.partition("=")
        env[k] = val

    vr = vram_report()
    if vr:
        used, total, apps = vr
        print("[%s] VRAM before launch: %d/%d MiB" % (tag, used, total))
        if apps:
            for a in apps:
                print("[%s]   held by: %s" % (tag, a))
        if total and used > total * 0.75:
            print("[%s] WARNING: >75%% of VRAM already used; a windowed "
                  "capture can go dark/input-dead under this pressure."
                  % tag, file=sys.stderr)

    pdir = os.path.join(outdir, tag)
    os.makedirs(pdir, exist_ok=True)
    launch = [BIN, "--width", str(args.width), "--height", str(args.height),
              "--mode", backend]
    if args.cam:
        # Start from a known-good pose. Measured with a live window: the
        # default spawn reads luma ~98 and village_hero ~100, while three
        # other candidate poses read 22-26, so the start pose is chosen, not
        # assumed.
        launch += ["--cam"] + str(args.cam).split()
    app = subprocess.Popen(launch,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, bufsize=1, env=env)
    log = []
    threading.Thread(target=lambda: [log.append(l.rstrip())
                                     for l in app.stdout], daemon=True).start()

    try:
        want = (args.width, args.height)
        win = geom = None
        t0 = time.time()
        while time.time() - t0 < 60:
            cands = [h for h in find_windows(root, TITLE) if h[0].id not in pre]
            exact = [h for h in cands if (h[1][2], h[1][3]) == want]
            pick = exact or sorted(cands, key=lambda h: -h[1][2] * h[1][3])
            if pick and pick[0][1][2] >= 640 and pick[0][1][3] >= 360:
                win, geom = pick[0][0], pick[0][1]
                break
            time.sleep(0.5)
        if geom is None:
            print("[%s] no window appeared" % tag, file=sys.stderr)
            return None
        print("[%s] window %s" % (tag, geom))

        inp = VFInput(title=TITLE, exclude=pre)
        t0 = time.time()
        while time.time() - t0 < 240:
            if any("layered_world: load" in l for l in log):
                break
            time.sleep(0.5)
        if not any("layered_world: load" in l for l in log):
            print("[%s] world never loaded" % tag, file=sys.stderr)
            return None
        print("[%s] world loaded (+%.1fs)" % (tag, time.time() - t0))

        # PROVE the capture target before trusting a single frame. An x11grab
        # reads the composited display, so an overlapping window yields a
        # well-formed image of the wrong surface and no pixel statistic can
        # tell you. Identity check instead.
        ok_t, tinfo = verify_capture_target(root, win, geom,
                                            d.screen().width_in_pixels,
                                            d.screen().height_in_pixels)
        print("[%s] capture target: app=%s geom=%s" % (tag, tinfo["app"], geom))
        print("[%s] top window at centre: %s" % (tag, tinfo.get("top")))
        if not ok_t:
            print("[%s] FAIL: the grab rect does not sample the app window "
                  "(something is on top of it). Refusing to record -- a "
                  "well-formed clip of the wrong surface is worse than none."
                  % tag, file=sys.stderr)
            if not args.force:
                return None

        # Wait for the rendered frame to be bright AND stable. The load log
        # line fires before the GPU has drawn anything, so recording from
        # there yields ~2-3 s of black at the head of the clip.
        #
        # A frame that never brightens is a HARD FAILURE, not something to
        # record anyway: a windowed capture can sit at luma ~27 while the
        # identical camera renders at ~124 offscreen (VRAM pressure starving
        # the swapchain), and writing that out produces a well-formed but
        # useless clip. Measured: a 13.6 s "successful" run that was dark
        # end to end. Fail loudly instead.
        x, y, w, h = geom
        probe = os.path.join(pdir, "_probe.png")
        prev, streak, m = None, 0, None
        t0 = time.time()
        ready = False
        while time.time() - t0 < args.ready_timeout:
            time.sleep(0.6)
            subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f",
                            "x11grab", "-video_size", "%dx%d" % (w, h),
                            "-i", ":0+%d,%d" % (x, y),
                            "-frames:v", "1", probe], check=True)
            m = _mean_luma(probe)
            if m is not None and m > 40:
                if prev is not None and abs(m - prev) < 0.5:
                    streak += 1
                    if streak >= 3:
                        ready = True
                        break
                else:
                    streak = 0
            prev = m
        print("[%s] frame ready: %s (luma %.1f, +%.1fs)"
              % (tag, ready, m if m else -1, time.time() - t0))
        if not ready:
            print("[%s] FAIL: window never rendered a bright stable frame "
                  "(last luma %.1f, floor 40). A windowed capture that stays "
                  "dark means the swapchain is starved - check VRAM "
                  "(nvidia-smi) before blaming the renderer. Offscreen "
                  "--shot of the same camera is the control."
                  % (tag, m if m else -1), file=sys.stderr)
            return None

        inp.focus()
        inp.center()
        time.sleep(0.4)

        # TAA off, for two reasons at once.
        #
        # (1) Test: the windowed path shares the HDR render, post pass and
        #     photorealism with the headless path, so only TAA (:5349) and the
        #     present differ. The present is not erroring, which leaves TAA.
        #     `N` toggles it and sets m_taaFirstFrame, forcing a full history
        #     reset -- so if the frame goes bright after this, TAA was it.
        # (2) Fix: a plausible TAA mechanism is a sticky NaN in the history
        #     (blend 0.92 carries 92% of each frame, and mix(cur, NaN, 0.92) is
        #     NaN, so one bad sample never washes out), which would be exposed
        #     by camera motion and would never recover without exactly this
        #     reset. Recording with TAA off sidesteps it either way.
        if args.taa_off:
            inp.tap("n", settle=1.0)
            time.sleep(0.8)
            t = [l for l in log if "TAA ->" in l]
            print("[%s] TAA toggle: %s"
                  % (tag, t[-1].split("TAA ->")[-1].strip() if t else "no log line"))

        # Exposure trim. The app has no env var for it (default m_exposure =
        # 1.15f); it is stepped by the `-` / `=` keys (x/1.1, floor 0.1) or the
        # sidebar slider. Inject the key rather than rebuilding the binary --
        # a relink here would invalidate whatever else is verifying the current
        # build. The app logs "exposure -> X", so we read the result back
        # instead of assuming the presses landed.
        if args.exposure_steps:
            for _ in range(args.exposure_steps):
                inp.tap("minus", settle=0.25)
            time.sleep(0.8)
            exp = [l for l in log if "exposure ->" in l]
            got = exp[-1].split("exposure ->")[-1].strip() if exp else "?"
            print("[%s] exposure after %d step(s) down: %s"
                  % (tag, args.exposure_steps, got))

        out = os.path.join(pdir, tag + ".mp4")
        # Stamp the premise INTO the clip, so the bright/dark outcome is
        # self-verifying: if a worker is reloaded mid-capture (the RAG
        # supervisor pulls a VLM on the next describe_image call) the log
        # says so, instead of becoming the next thing two sessions argue
        # about. One nvidia-smi call.
        vr0 = vram_report()
        if vr0:
            vtxt = "vram_used_mib=%d/%d holders=%s" % (
                vr0[0], vr0[1], ";".join(vr0[2]) or "none")
        else:
            vtxt = "vram_unknown"
        print("[%s] %s" % (tag, vtxt))
        with open(os.path.join(pdir, tag + ".vram.txt"), "w") as fh:
            fh.write("clip_start_utc=%s\n%s\n"
                     % (time.strftime("%Y-%m-%dT%H:%M:%S"), vtxt))
        rec = subprocess.Popen(
            ["ffmpeg", "-y", "-loglevel", "error", "-f", "x11grab",
             "-framerate", str(args.fps),
             "-video_size", "%dx%d" % (w, h), "-i", ":0+%d,%d" % (x, y),
             "-t", str(seconds),
             "-c:v", "libx264", "-pix_fmt", "yuv420p",
             "-preset", "veryfast", "-crf", "20",
             "-metadata", "comment=" + vtxt,
             out],
            stderr=subprocess.DEVNULL)
        print("[%s] recording %.0fs -> %s" % (tag, seconds, out))

        # Joint 1 Hz time series: nvidia-smi memory.used AND the sampled frame
        # luma on the SAME clock. Two datasets disagree (a clip that is dark
        # and static from frame 0 vs a window that is bright at ~26 s and dark
        # at ~30 s), and the cheap discriminator is whether the transition
        # lands on a memory event. Do NOT reconcile them by averaging - if
        # VRAM is flat across the transition, pressure is exonerated and the
        # cause is present/allocation timing instead.
        ts_path = os.path.join(pdir, tag + "_series.csv")
        ts_f = open(ts_path, "w")
        ts_f.write("t_s,vram_used_mib,luma,window_id\n")
        t_start = time.time()
        series = []
        dark = []
        stop_watch = threading.Event()
        floor = 40.0

        def watch():
            base = None
            while not stop_watch.wait(1.0):
                probe_now = os.path.join(pdir, "_watch.png")
                try:
                    subprocess.run(
                        ["ffmpeg", "-y", "-loglevel", "error", "-f", "x11grab",
                         "-video_size", "%dx%d" % (w, h), "-i", ":0+%d,%d" % (x, y),
                         "-frames:v", "1", probe_now],
                        check=True, capture_output=True)
                except Exception:
                    continue
                m = _mean_luma(probe_now)
                if m is None:
                    continue
                vr = vram_report()
                v = vr[0] if vr else -1
                series.append((round(time.time() - t_start, 1), v, round(m, 1)))
                try:
                    ts_f.write("%.1f,%d,%.1f,%s\n"
                               % (time.time() - t_start, v, m, hex(win.id)))
                    ts_f.flush()
                except Exception:
                    pass
                if base is None:
                    base = m
                if base <= floor or m < base * 0.45:
                    dark.append((base, m))
                    return

        threading.Thread(target=watch, daemon=True).start()

        took = play(inp, log=lambda s: print("[%s]   %s" % (tag, s)),
                    budget=seconds, fill=seconds)
        stop_watch.set()
        try:
            ts_f.close()
        except Exception:
            pass
        rec.terminate()
        try:
            rec.wait(timeout=20)
        except subprocess.TimeoutExpired:
            rec.kill()
        print("[%s] tour took %.1fs" % (tag, took))
        if dark:
            print("[%s] ABORT: frame collapsed from luma %.1f to %.1f during "
                  "recording - clip is dark, discard it"
                  % (tag, dark[0][0], dark[0][1]), file=sys.stderr)
        if series:
            vmin = min(s[1] for s in series)
            vmax = max(s[1] for s in series)
            lmin = min(s[2] for s in series)
            lmax = max(s[2] for s in series)
            print("[%s] series (%d samples) -> %s" % (tag, len(series), ts_path))
            print("[%s]   vram %d..%d MiB (swing %d)   luma %.1f..%.1f"
                  % (tag, vmin, vmax, vmax - vmin, lmin, lmax))
            if vmax - vmin < 64:
                print("[%s]   NOTE: VRAM is FLAT (<64 MiB swing) across the "
                      "run - if luma moved, memory pressure is exonerated "
                      "and the cause is present/allocation timing."
                      % tag)
        return out
    finally:
        app.terminate()
        try:
            out_txt, _ = app.communicate(timeout=15)
        except subprocess.TimeoutExpired:
            app.kill()
            out_txt = ""
        for l in [l for l in out_txt.splitlines()
                  if "layered_world: load" in l or "render mode" in l][-4:]:
            print("[%s] %s" % (tag, l))


def run_dual(args, outdir):
    """Record the same tour once per backend and concatenate to one clip."""
    from vf_tour import total_seconds
    os.makedirs(outdir, exist_ok=True)
    clips = []
    passes = [("splat", "splat"), ("voxel", "svo")]
    for i, (tag, backend) in enumerate(passes, 1):
        print("\n=== pass %d/%d: %s ===" % (i, len(passes), tag))
        # The brightness gate is intermittent: observed one launch bright in
        # 12.9 s and another never bright in 121 s, same binary and same GPU.
        # So a failed gate is a retry, not a lost reel -- but cap the retries
        # rather than spinning forever.
        for attempt in range(1, args.retries + 2):
            if attempt > 1:
                print("[%s] retry %d/%d" % (tag, attempt - 1, args.retries))
                time.sleep(3)
            p = record_pass(args, outdir, tag, backend, [], args.seconds)
            if p:
                clips.append(p)
                break
            print("[%s] attempt %d produced no clip" % (tag, attempt))
        else:
            print("[dual] pass %s failed after %d attempts"
                  % (tag, args.retries + 1), file=sys.stderr)
    if not clips:
        return 1

    final = os.path.join(outdir, "demo_dual.mp4")
    lst = os.path.join(outdir, "concat.txt")
    with open(lst, "w") as f:
        for c in clips:
            f.write("file '%s'\n" % os.path.abspath(c))
    print("\n[dual] concatenating %d passes -> %s" % (len(clips), final))
    subprocess.run(
        ["ffmpeg", "-y", "-loglevel", "error", "-f", "concat",
         "-safe", "0", "-i", lst,
         "-c:v", "libx264", "-pix_fmt", "yuv420p",
         "-preset", "veryfast", "-crf", "20", final], check=True)
    print("[dual] done -> %s" % final)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mode",
                    choices=["shots", "live", "dual"], default="shots")
    ap.add_argument("--seconds", type=float, default=60.0,
                    help="per-pass length for --mode dual")
    ap.add_argument("--ready-timeout", type=float, default=120.0,
                    help="seconds to wait for a bright stable frame before "
                         "failing the pass (a permanently dark window means "
                         "the swapchain is starved, not that the render is slow)")
    ap.add_argument("--out", default="/tmp/opencode/demo")
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=720)
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--backend", choices=["splat", "svo"], default="splat",
                    help="backend for --mode live only")
    ap.add_argument("--variants", nargs="*", choices=[v[0] for v in VARIANTS],
                    help="subset of variants to render (default: all). "
                         "Each is one app launch / world load.")
    ap.add_argument("--sheet", action="store_true",
                    help="build a per-shot contact sheet comparing variants")
    # WHY these escape hatches exist: a check that can only fail loudly puts
    # the next person in a bad spot. When a clip trips it they have two moves
    # -- delete the check, or fix the clip -- and a hard failure makes
    # deleting the check look like the cheaper one. --no-check / --lenient let
    # the check LOSE an argument without losing its code. Keep them: a gate
    # that cannot be argued with gets deleted, and then it gates nothing.
    ap.add_argument("--check", action="store_true", default=True,
                    help="apply visual_check.py's coverage band + sky probe "
                         "to every rendered keyframe (default on; this is "
                         "what catches a keyframe framing empty ground)")
    ap.add_argument("--no-check", dest="check", action="store_false",
                    help="skip the per-keyframe acceptance check")
    ap.add_argument("--lenient", action="store_true",
                    help="report failing keyframes but still exit 0")
    ap.add_argument("--force", action="store_true",
                    help="record even if the grab rect does not provably "
                         "sample the app window (not recommended)")
    ap.add_argument("--cam", default="0 7 0 15 3 15",
                    help="start pose for the live/dual pass, as "
                         "'x y z tx ty tz'. Default is village_hero, measured "
                         "at luma ~100 in a live window; the default spawn "
                         "also reads ~98. Pass an empty string to use the "
                         "app's own spawn.")
    ap.add_argument("--exposure-steps", type=int, default=3,
                    help="press '-' this many times before recording "
                         "(exposure is x/1.1 per press, default 1.15). 3 steps "
                         "lands near 0.86. 0 disables.")
    ap.add_argument("--taa-off", action="store_true", default=True,
                    help="press N to disable TAA before recording (default "
                         "on). TAA is the only windowed-only stage left after "
                         "eliminating the shared render/post/photorealism "
                         "path, and a sticky NaN in its history would be "
                         "exposed by camera motion and never recover.")
    ap.add_argument("--taa-on", dest="taa_off", action="store_false",
                    help="leave TAA enabled (keeps the windowed-only stage in "
                         "the capture)")
    ap.add_argument("--retries", type=int, default=2,
                    help="retries per pass in --mode dual when the brightness "
                         "gate fails (the dark state is intermittent)")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--animtime", type=float, default=0.0,
                    help="freeze animation at this time (deterministic frames)")
    ap.add_argument("--sun", nargs=2, type=float, metavar=("ELEV", "AZIM"),
                    help="override the sun, e.g. --sun 34 238")
    ap.add_argument("--ffmpeg", action="store_true",
                    help="also stitch the shot frames into demo.mp4")
    ap.add_argument("--list", action="store_true",
                    help="print the shotlist and variants, then exit")
    args = ap.parse_args()

    if not os.path.exists(BIN):
        print(f"missing {BIN} - run: ninja -C build", file=sys.stderr)
        return 1

    if args.list:
        print("variants (one app launch each):")
        for tag, envset, backend in VARIANTS:
            print(f"  {tag:12s} mode={backend:6s} "
                  f"{' '.join(envset) or '(defaults)'}")
        print("\nkeyframes:")
        for name, cx, cy, cz, tx, ty, tz in SHOTS:
            print(f"  {name:18s} cam=({cx:6.1f},{cy:5.1f},{cz:6.1f})  "
                  f"target=({tx:5.1f},{ty:5.1f},{tz:5.1f})")
        return 0

    if args.mode == "live":
        return run_live(args, args.out)
    if args.mode == "dual":
        return run_dual(args, args.out)
    return run_shots(args, args.out)


if __name__ == "__main__":
    sys.exit(main())
