#!/usr/bin/env python3
"""test_inject_iso.py - validate XTEST input with NO other Voxelforge running.

The multi-window test is unreliable when leftover app processes are alive:
several windows share the WM_NAME "Voxelforge" at different sizes, so window
binding and screen capture race each other. This variant refuses to run if
any other Voxelforge process is present, so the result is unambiguous.

Run:  python3 tools/test_inject_iso.py
"""
import os
import subprocess
import sys
import time

import numpy as np
from PIL import Image
from Xlib import X, display
from Xlib.ext import xtest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "voxelforge")
WORK = "/tmp/opencode/inject_iso"
TITLE = "Voxelforge"


def find_windows(root, title):
    hits = []
    stack = [root]
    while stack:
        w = stack.pop()
        try:
            if w.get_wm_name() == title:
                g = w.get_geometry()
                tr = root.translate_coords(w, 0, 0)
                pid = None
                try:
                    p = w.get_full_property(
                        root.display.intern_atom("_NET_WM_PID"), X.AnyPropertyType)
                    if p:
                        pid = p.value[0]
                except Exception:
                    pass
                hits.append((w, (tr.x, tr.y, g.width, g.height), pid))
            stack.extend(w.query_tree().children)
        except Exception:
            pass
    return hits


def wait_for_clear(root, samples=5, gap=2.0, timeout=120.0):
    """Block until no Voxelforge window is present for `samples` in a row.

    A single "no window" sample is not enough: a driver like live_edit_check
    renders sequentially, so the display is briefly empty between shots and a
    one-shot check races straight into the next render. Requiring several
    consecutive clear samples removes that gap.
    """
    import os
    t0 = time.time()
    streak = 0
    while time.time() - t0 < timeout:
        live = []
        for w, g, pid in find_windows(root, TITLE):
            alive = True
            if pid:
                try:
                    os.kill(pid, 0)
                except OSError:
                    alive = False
            if alive:
                live.append((w, g, pid))
        if not live:
            streak += 1
            if streak >= samples:
                return True
        else:
            streak = 0
        time.sleep(gap)
    return False


def grab(geom, path):
    x, y, w, h = geom
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "x11grab",
                    "-video_size", f"{w}x{h}", "-i", f":0+{x},{y}",
                    "-frames:v", "1", path], check=True)
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)


def frac_diff(a, b, thresh=12):
    if a.shape != b.shape:
        return 1.0
    return float((np.abs(a - b).max(axis=2) > thresh).mean())


def main():
    os.makedirs(WORK, exist_ok=True)
    d = display.Display(":0")
    root = d.screen().root

    # Guard on *live windows*, not on processes. An offscreen --shot render
    # is a build/voxelforge process too, and it DOES own a titled window
    # (it just never reads it back), so only a window-based guard is
    # meaningful. Windows whose owning pid is gone are ignored as stale.
    if not wait_for_clear(root):
        print("REFUSING to run - a live Voxelforge window is still open:")
        for w, g, pid in find_windows(root, TITLE):
            print(f"   {hex(w.id)} {g} pid={pid}")
        print("These share the WM_NAME and break window binding/capture.")
        print("If a driver is running, wait for it to finish.")
        return 2
    pre = set()

    env = dict(os.environ, VF_NO_OVERLAY="1",
               VF_OVERLAY_PATH=os.path.join(WORK, "o.vxw"))
    print("launching isolated app")
    app = subprocess.Popen([BIN, "--width", "1280", "--height", "720"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, bufsize=1, env=env)

    try:
        win = geom = None
        want = (1280, 720)
        t0 = time.time()
        while time.time() - t0 < 60:
            # Pick the NEW window that matches the size we asked for. A plain
            # hits[0] grabs whichever window a DFS happens to reach first,
            # which is routinely a tiny leftover from an earlier run and makes
            # the whole capture meaningless.
            cands = [h for h in find_windows(root, TITLE) if h[0].id not in pre]
            exact = [h for h in cands if (h[1][2], h[1][3]) == want]
            pick = exact or sorted(cands, key=lambda h: -h[1][2] * h[1][3])
            if pick and (pick[0][1][2] >= 640 and pick[0][1][3] >= 360):
                win, geom, _pid = pick[0]
                break
            time.sleep(0.5)
        if win is None:
            print("FAIL: no suitable window appeared")
            return 1
        print(f"  window {hex(win.id)} geom {geom}")
        if (geom[2], geom[3]) != want:
            print(f"  NOTE: requested {want}, got {(geom[2], geom[3])} "
                  f"(WM resize) - continuing with the real size")

        # wait for the world to load (swapchain line is the reliable signal)
        loaded = False
        t0 = time.time()
        while time.time() - t0 < 180:
            app.poll()
            # read available log non-blockingly via a helper thread
            time.sleep(0.5)
            # fall back: probe stability of the frame
            a = grab(geom, os.path.join(WORK, "probe.png"))
            if a.mean() > 25:      # loaded scene is far brighter than black
                loaded = True
                if time.time() - t0 > 6:
                    break
        print(f"  scene appears loaded: {loaded} (mean {a.mean():.1f})")

        # focus + centre
        try:
            win.configure(stack_mode=X.Above)
            d.set_input_focus(win.id, X.RevertToParent, X.CurrentTime)
        except Exception as e:
            print("  focus warn:", e)
        d.sync()
        root.warp_pointer(geom[0] + geom[2] // 2, geom[1] + geom[3] // 2)
        d.sync()
        time.sleep(1.0)

        base = grab(geom, os.path.join(WORK, "base.png"))
        print(f"  baseline mean {base.mean():.2f}")

        # settle check: two idle frames should be nearly identical
        id1 = grab(geom, os.path.join(WORK, "idle1.png"))
        time.sleep(0.5)
        id2 = grab(geom, os.path.join(WORK, "idle2.png"))
        idle = frac_diff(id1, id2)
        print(f"  idle noise fraction {idle:.4f} (animated water expected)")

        # ---- keyboard -----------------------------------------------
        # Animated water means two idle frames already differ, so compare
        # against the measured idle noise rather than a fixed threshold.
        #
        # glfwGetKey reads GLFW's INTERNAL key state, which is only updated by
        # KeyPress EVENTS delivered to the focused window -- NOT by the global
        # X keymap. A set keymap bit therefore proves the server saw the
        # press, not that the app did. Re-assert focus immediately before the
        # burst and verify it is still ours while the key is held.
        results = {}
        try:
            d.set_input_focus(win.id, X.RevertToParent, X.CurrentTime)
        except Exception as e:
            print("  focus warn:", e)
        d.sync()
        time.sleep(0.2)
        print(f"  focus before press: {d.get_input_focus().focus} "
              f"(ours={hex(win.id)})")

        wcode = d.keysym_to_keycode(0x77)
        km = d.query_keymap()
        held_before = (km[wcode // 8] >> (wcode % 8)) & 1
        xtest.fake_input(d, X.KeyPress, detail=wcode, time=X.CurrentTime)
        d.sync()
        time.sleep(0.15)
        km = d.query_keymap()
        held = (km[wcode // 8] >> (wcode % 8)) & 1
        foc = d.get_input_focus().focus
        print(f"  W keymap: before={held_before} during={held}; "
              f"focus now {foc} ours={foc.id == win.id}")
        time.sleep(2.0)
        moved = grab(geom, os.path.join(WORK, "moved.png"))
        xtest.fake_input(d, X.KeyRelease, detail=wcode, time=X.CurrentTime)
        d.sync()
        f_kb = frac_diff(base, moved)
        thresh = max(0.20, idle * 2)
        print(f"  W-forward diff {f_kb:.3f} (threshold {thresh:.3f})")
        results["keyboard"] = bool(held) and f_kb > thresh

        # ---- mouse look -----------------------------------------------
        # glfwGetMouseButton only reports PRESS while the pointer is INSIDE
        # the client rect, and the app applies look deltas only while RMB is
        # down. Re-assert focus and re-centre first: anything that moved the
        # pointer since the keyboard test (or a WM that reclaimed it) makes
        # RMB-look a silent no-op.
        try:
            d.set_input_focus(win.id, X.RevertToParent, X.CurrentTime)
        except Exception:
            pass
        d.sync()
        cx, cy = geom[0] + geom[2] // 2, geom[1] + geom[3] // 2
        root.warp_pointer(cx, cy)
        d.sync()
        time.sleep(0.4)
        before_ptr = root.query_pointer()
        in_win = (geom[0] <= before_ptr.root_x < geom[0] + geom[2]
                  and geom[1] <= before_ptr.root_y < geom[1] + geom[3])
        print(f"  pointer ({before_ptr.root_x},{before_ptr.root_y}) "
              f"inside window: {in_win}")
        if not in_win:
            print("  FAIL: pointer is not over the window; RMB would not read")
            results["look"] = False
        else:
            xtest.fake_input(d, X.ButtonPress, detail=3, time=X.CurrentTime)
            d.sync()
            time.sleep(0.2)
            # NOTE: this X server reports button-mask bits offset by 8
            # (detail=3 -> bit 10, not bit 2), so do not assert on the mask
            # bit; the event detail is what GLFW maps to MOUSE_BUTTON_RIGHT.
            steps = 120
            for i in range(steps):
                root.warp_pointer(cx + i * 2, cy)
                d.sync()
                time.sleep(1 / 60)
            xtest.fake_input(d, X.ButtonRelease, detail=3, time=X.CurrentTime)
            d.sync()
            time.sleep(0.8)
            looked = grab(geom, os.path.join(WORK, "looked.png"))
            f_look = frac_diff(moved, looked)
            after_ptr = root.query_pointer()
            print(f"  pointer {before_ptr.root_x},{before_ptr.root_y} -> "
                  f"{after_ptr.root_x},{after_ptr.root_y}")
            print(f"  look diff {f_look:.3f}")
            results["look"] = f_look > max(0.20, idle * 2)

        print()
        for k, v in results.items():
            print(f"  {k:9s} {'PASS' if v else 'FAIL'}")
        print(f"  (idle noise {idle:.3f}, threshold {max(0.20, idle * 2):.3f})")
        ok = all(results.values())
        print("\nPASS - injected input drives the renderer." if ok
              else "\nFAIL - input not observed.")
        return 0 if ok else 1
    finally:
        app.terminate()
        try:
            out, _ = app.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            app.kill()
            out = ""
        tail = [l for l in out.splitlines()
                if "Swapchain" in l or "surfels" in l or "layered_world" in l]
        print("\n--- app log ---")
        print("\n".join(tail[-6:]))


if __name__ == "__main__":
    sys.exit(main())
