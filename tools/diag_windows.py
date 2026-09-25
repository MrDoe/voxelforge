#!/usr/bin/env python3
"""diag_windows.py - enumerate every window named 'Voxelforge'.

Prints id, WM_STATE (normal/iconic), geometry and root position for each, so
we can tell the real 1280x720 window from stale leftovers or from other runs
that happen to share the title.
"""
import sys

from Xlib import display
from Xlib import Xatom  # noqa: F401

WM_STATE = 39 + 1  # Xatom.WM_STATE is 39; WM_STATE itself is 39? use atom lookup


def main():
    d = display.Display(":0")
    root = d.screen().root
    wm_state = d.intern_atom("WM_STATE")
    net_wm_pid = d.intern_atom("_NET_WM_PID")
    print("WM_STATE atom", wm_state, " _NET_WM_PID atom", net_wm_pid)

    rows = []

    def walk(w, depth):
        try:
            nm = w.get_wm_name()
        except Exception:
            nm = None
        if nm and "oxel" in nm:
            try:
                g = w.get_geometry()
                tr = root.translate_coords(w, 0, 0)
            except Exception:
                g = tr = None
            state = None
            try:
                p = w.get_full_property(wm_state, 0)
                if p:
                    state = p.value[0]
            except Exception:
                pass
            pid = None
            try:
                p = w.get_full_property(net_wm_pid, 0)
                if p:
                    pid = p.value[0]
            except Exception:
                pass
            rows.append((depth, w.id, nm, g, tr, state, pid))
        try:
            for c in w.query_tree().children:
                walk(c, depth + 1)
        except Exception:
            pass

    for c in root.query_tree().children:
        walk(c, 0)

    # WM_STATE values: 0=Withdrawn 1=Normal 3=Iconic
    names = {0: "Withdrawn", 1: "Normal", 3: "Iconic"}
    for depth, wid, nm, g, tr, state, pid in rows:
        geo = (f"{g.width}x{g.height}" if g else "?")
        pos = (f"({tr.x},{tr.y})" if tr else "?")
        print(f"  {hex(wid):>10s} {nm[:34]:<36s} {geo:>10s} root={pos:<14s} "
              f"state={names.get(state, state)} pid={pid}")


if __name__ == "__main__":
    sys.exit(main())
