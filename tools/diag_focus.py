#!/usr/bin/env python3
"""diag_focus.py - why does RMB-look not register?

Two candidate causes, both measured here:
  (a) the WM steals / reverts the pointer, so it never sits over the window
      (glfwGetMouseButton only reports PRESS while the pointer is inside the
      client rect -- that alone makes RMB-look a no-op)
  (b) the app window does not hold X input focus, so XTEST key/button events
      are delivered to some other client
"""
import os
import subprocess
import sys
import time

from Xlib import X, display
from Xlib.ext import xtest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "voxelforge")
TITLE = "Voxelforge"


def find(root, title):
    stack, hits = [root], []
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


def inside(p, g):
    return g[0] <= p[0] < g[0] + g[2] and g[1] <= p[1] < g[1] + g[3]


def main():
    d = display.Display(":0")
    root = d.screen().root
    env = dict(os.environ, VF_NO_OVERLAY="1",
               VF_OVERLAY_PATH="/tmp/opencode/focus.vxw")
    app = subprocess.Popen([BIN, "--width", "1280", "--height", "720"],
                           stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, env=env)
    try:
        win = geom = None
        t0 = time.time()
        while time.time() - t0 < 40:
            hits = find(root, TITLE)
            if hits:
                win, geom = hits[0]
                break
            time.sleep(0.5)
        if win is None:
            print("no window")
            return 1
        print(f"window {hex(win.id)} geom {geom}")

        time.sleep(18)   # let the world load

        print("\n-- focus --")
        f = d.get_input_focus().focus
        print(f"  focus is {f}  (ours={hex(win.id)})  match={f.id == win.id}")

        print("\n-- pointer landing --")
        for attempt in range(4):
            tx, ty = geom[0] + geom[2] // 2, geom[1] + geom[3] // 2
            root.warp_pointer(tx, ty)
            d.sync()
            time.sleep(0.15)
            p = root.query_pointer()
            print(f"  warp->({tx},{ty})  landed=({p.root_x},{p.root_y})  "
                  f"inside={inside((p.root_x, p.root_y), geom)}")
            if not inside((p.root_x, p.root_y), geom):
                print("    ^^ pointer did NOT land inside the window")

        print("\n-- does the WM restore focus after we set it? --")
        try:
            d.set_input_focus(win.id, X.RevertToParent, X.CurrentTime)
        except Exception as e:
            print("  set_input_focus error:", e)
        d.sync()
        time.sleep(0.4)
        f2 = d.get_input_focus().focus
        print(f"  after re-assert: {f2} match={f2.id == win.id}")

        print("\n-- pointer while RMB held (glfwGetMouseButton condition) --")
        tx, ty = geom[0] + geom[2] // 2, geom[1] + geom[3] // 2
        root.warp_pointer(tx, ty)
        d.sync()
        xtest.fake_input(d, X.ButtonPress, detail=3, time=X.CurrentTime)
        d.sync()
        time.sleep(0.3)
        q = root.query_pointer()
        mask = q.mask
        print(f"  pointer=({q.root_x},{q.root_y}) inside={inside((q.root_x, q.root_y), geom)}")
        print(f"  button mask=0x{mask:x}  RMB(bit2)={bool(mask & (1 << 2))}")
        qw = win.query_pointer()
        print(f"  win.query_pointer mask=0x{qw.mask:x} "
              f"RMB={bool(qw.mask & (1 << 2))}  same_screen={qw.same_screen}")
        xtest.fake_input(d, X.ButtonRelease, detail=3, time=X.CurrentTime)
        d.sync()
        return 0
    finally:
        app.terminate()
        try:
            app.wait(timeout=8)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    sys.exit(main())
