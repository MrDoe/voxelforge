#!/usr/bin/env python3
"""test_inject.py - prove injected X11 input reaches the Voxelforge window.

Launches the app visible at 1280x720, waits for the world to actually load,
then A/B-compares frames to prove each input class lands:

  1. keyboard: hold W 2 s        -> camera moves forward
  2. mouse-look: RMB + 500 px    -> view rotates
  3. hotkey: F (svo) then F      -> renderer flips and returns
  4. edit tool: C/A/M first (must be inert in View mode), then Tab
                                 -> sidebar switches to the Edit panel

World load is ~17 s (per AGENTS.md) and the GLFW window exists long before
it, so readiness is gated on the app's own "layered_world: load" log line
plus a brightness-stability check. Diffs are computed with numpy -- an
earlier version parsed ffmpeg blackframe output and reported identical
numbers for every case, which masked the real problem.

Run:  python3 tools/test_inject.py
"""

import os
import subprocess
import sys
import threading
import time

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vf_input import VFInput  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "voxelforge")
WORK = "/tmp/opencode/inject"
TITLE = "Voxelforge"
GEOM = (0, 0, 0, 0)


# --------------------------------------------------------------- app launch

class App:
    """Voxelforge subprocess with a live, threadable log buffer."""

    def __init__(self, pre_existing=frozenset()):
        os.makedirs(WORK, exist_ok=True)
        self.env = dict(os.environ)
        self.env["VF_NO_OVERLAY"] = "1"
        self.env["VF_OVERLAY_PATH"] = os.path.join(WORK, "overlay.vxw")
        self.lines = []
        self.lock = threading.Lock()
        self.pre_existing = pre_existing
        self.proc = subprocess.Popen(
            [BIN, "--width", "1280", "--height", "720"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1, env=self.env)
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        assert self.proc.stdout is not None
        for line in self.proc.stdout:
            with self.lock:
                self.lines.append(line.rstrip())

    def log(self):
        with self.lock:
            return list(self.lines)

    def saw_load(self):
        return any("layered_world: load" in l for l in self.log())

    def close(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()


# ------------------------------------------------------------------ capture

def grab(name):
    """ffmpeg x11grab of the window rect -> PNG."""
    global GEOM
    x, y, w, h = GEOM
    path = os.path.join(WORK, name)
    subprocess.run(
        ["ffmpeg", "-y", "-loglevel", "error", "-f", "x11grab",
         "-video_size", f"{w}x{h}", "-i", f":0+{x},{y}",
         "-frames:v", "1", path], check=True)
    return path


def load_rgb(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)


def diff_frac(a_path, b_path, thresh=12):
    """Fraction of pixels whose max channel differs by more than `thresh`."""
    a, b = load_rgb(a_path), load_rgb(b_path)
    if a.shape != b.shape:
        return 1.0
    return float((np.abs(a - b).max(axis=2) > thresh).mean())


def mean_luma(path):
    return float(load_rgb(path).mean())


def wait_stable(settle=0.6, stable_n=3, timeout=120.0):
    """Block until the rendered frame stops changing.

    The world load stalls the frame loop for many seconds; capturing during
    that window yields a near-black frame that makes every later A/B look
    like a huge change. Poll until consecutive luma means agree.
    """
    t0 = time.time()
    prev = None
    n = 0
    while time.time() - t0 < timeout:
        time.sleep(settle)
        m = mean_luma(grab("_probe.png"))
        if prev is not None and abs(m - prev) < 0.05:
            n += 1
            if n >= stable_n:
                print(f"      frame stable at luma {m:.2f} "
                      f"(+{time.time() - t0:.1f}s)")
                return True
        else:
            n = 0
        prev = m
    print(f"      WARNING: frame never stabilised (last luma {prev})")
    return False


# --------------------------------------------------------------------- main

def main():
    global GEOM
    # Snapshot same-titled windows BEFORE launching. A crashed earlier run
    # can leave a stale Voxelforge window, and X recycles window ids, so a
    # plain title search can bind to the wrong window and capture the wrong
    # pixels -- which reads as "input does nothing".
    pre = VFInput.snapshot_titles()
    if pre:
        print(f"      (ignoring {len(pre)} pre-existing Voxelforge window(s))")

    print("[1/5] launching", BIN)
    app = App(pre_existing=pre)

    try:
        inp = None
        t0 = time.time()
        while time.time() - t0 < 40:
            try:
                inp = VFInput(title=TITLE, exclude=pre)
                break
            except SystemExit:
                time.sleep(0.5)
        if inp is None:
            print("FAIL: window never appeared")
            return 1
        GEOM = inp.geometry()
        print(f"      window {hex(inp.win.id)} at {GEOM}")
        if GEOM[2] < 640 or GEOM[3] < 360:
            print("FAIL: window implausibly small / off-screen:", GEOM)
            return 1

        # --- wait for the world to finish loading ---
        t0 = time.time()
        while time.time() - t0 < 180 and not app.saw_load():
            time.sleep(0.5)
        if not app.saw_load():
            print("FAIL: world never finished loading")
            return 1
        print(f"      world loaded after {time.time() - t0:.1f}s")
        wait_stable()

        inp.center()
        time.sleep(1.0)
        base = grab("base.png")
        print(f"      baseline luma {mean_luma(base):.2f}")

        results = {}

        # ---- 1. keyboard ------------------------------------------------
        print("[2/5] keyboard: holding W for 2 s")
        inp.hold("w", 2.0, settle=0.8)
        time.sleep(0.6)
        a1 = grab("after_w.png")
        d = diff_frac(base, a1)
        results["keyboard"] = d > 0.30
        print(f"      moved fraction {d:.3f} -> "
              f"{'PASS' if results['keyboard'] else 'FAIL'}")

        # ---- 2. mouse look ---------------------------------------------
        print("[3/5] mouse: RMB held + 500 px right sweep over 2 s")
        inp.look_begin()
        inp.look(500, 0, 2.0)
        inp.look_end()
        time.sleep(0.8)
        a2 = grab("after_look.png")
        d2 = diff_frac(a1, a2)
        results["look"] = d2 > 0.30
        print(f"      moved fraction {d2:.3f} -> "
              f"{'PASS' if results['look'] else 'FAIL'}")

        # ---- 3. hotkey round trip ---------------------------------------
        print("[4/5] hotkey: F -> SVO, F -> splat")
        before = a2
        inp.tap("f", settle=1.2)
        svo = grab("svo.png")
        d_svo = diff_frac(before, svo)
        inp.tap("f", settle=1.2)
        back = grab("back.png")
        d_back = diff_frac(before, back)
        # water/foliage animate, so "back" is close-but-not-identical;
        # a real renderer switch is a large, structural change.
        results["hotkey"] = d_svo > 0.20 and d_back < d_svo * 0.5
        print(f"      splat->svo {d_svo:.3f}, back {d_back:.3f} -> "
              f"{'PASS' if results['hotkey'] else 'FAIL'}")

        # ---- 4. edit panel (Tab is the only View/Edit switch) ------------
        print("[5/5] View-mode mode keys inert; Tab switches sidebar to Edit")
        # Regression guard: C/A/D/S/M used to arm the brush when disarmed, so
        # a stray press while flying dragged the user into Edit mode. They
        # must do nothing now; only Tab may switch.
        inp.tap("c", hold=0.25, settle=0.4)
        inp.tap("a", hold=0.25, settle=0.4)
        inp.tap("m", hold=0.25, settle=0.4)
        inert = grab("inert.png")
        a, b = load_rgb(back), load_rgb(inert)
        strip_i = float((np.abs(a[:, :300] - b[:, :300]).max(axis=2) > 12).mean())
        no_arm = not any("edit tool -> active" in l or "edit mode ->" in l
                         for l in app.log())
        results["view_mode_inert"] = strip_i < 0.02 and no_arm
        print(f"      left-strip diff {strip_i:.3f}, no arm log {no_arm} -> "
              f"{'PASS' if results['view_mode_inert'] else 'FAIL'}")

        inp.tap("tab", settle=1.0)
        pan = grab("panel.png")
        dp = diff_frac(inert, pan)
        # panel is a ~300 px left strip; restrict the metric to that region
        a, b = load_rgb(inert), load_rgb(pan)
        strip = float((np.abs(a[:, :300] - b[:, :300]).max(axis=2) > 12).mean())
        results["panel"] = strip > 0.20
        print(f"      full diff {dp:.3f}, left-strip diff {strip:.3f} -> "
              f"{'PASS' if results['panel'] else 'FAIL'}")
        inp.tap("tab", settle=0.5)

        print()
        for k, v in results.items():
            print(f"  {k:9s} {'PASS' if v else 'FAIL'}")
        ok = all(results.values())
        print("\nALL PASS - injected input drives the app."
              if ok else "\nFAILURES - do not trust the recorder.")
        return 0 if ok else 1
    finally:
        app.close()
        print("\n--- app log tail ---")
        for line in app.log()[-10:]:
            print(line)


if __name__ == "__main__":
    sys.exit(main())
