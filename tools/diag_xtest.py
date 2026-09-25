#!/usr/bin/env python3
"""diag_xtest.py - determine whether XTEST keyboard/button injection works.

Keymap indexing gotcha: XQueryKeymap returns 32 BYTES, where byte N holds the
bits for keycodes 8N..8N+7, least-significant bit first. Indexing the list by
the raw keycode (km[25]) reads a byte that is almost always zero, which looks
exactly like "XTEST keys do nothing". Test keycode K as
(km[K // 8] >> (K % 8)) & 1.

Note that mouse buttons do not appear in the keymap at all; they are tracked
separately by XQueryPointer's mask, so a keymap probe cannot confirm buttons.
"""
import os
import subprocess
import sys
import time

from Xlib import X, display
from Xlib.ext import xtest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "voxelforge")


def key_is_down(d, code):
    km = d.query_keymap()
    return bool((km[code // 8] >> (code % 8)) & 1)


def main():
    env = dict(os.environ, VF_NO_OVERLAY="1")
    app = subprocess.Popen([BIN, "--width", "800", "--height", "600"],
                           stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, env=env)
    time.sleep(8)
    d = display.Display(":0")
    root = d.screen().root
    try:
        lo = d.display.info.min_keycode
        print(f"keycode range {lo}..{d.display.info.max_keycode}, "
              f"keymap bytes {len(d.query_keymap())}")

        for name, sym in (("w", 0x77), ("a", 0x61), ("c", 0x63), ("f", 0x66)):
            code = d.keysym_to_keycode(sym)
            before = key_is_down(d, code)
            xtest.fake_input(d, X.KeyPress, detail=code, time=X.CurrentTime)
            d.sync()
            time.sleep(0.2)
            during = key_is_down(d, code)
            xtest.fake_input(d, X.KeyRelease, detail=code, time=X.CurrentTime)
            d.sync()
            time.sleep(0.15)
            after = key_is_down(d, code)
            verdict = "OK" if (not before and during and not after) else "BAD"
            print(f"  {name}: keycode {code:3d} "
                  f"before={int(before)} during={int(during)} "
                  f"after={int(after)}  {verdict}")

        # buttons are not in the keymap; report only that the calls succeed
        for b, label in ((1, "LMB"), (3, "RMB")):
            xtest.fake_input(d, X.ButtonPress, detail=b, time=X.CurrentTime)
            d.sync()
            xtest.fake_input(d, X.ButtonRelease, detail=b, time=X.CurrentTime)
            d.sync()
            print(f"  {label}: XTEST calls accepted (not keymap-visible)")
        print(f"  pointer at {root.query_pointer().root_x},"
              f"{root.query_pointer().root_y}")
    finally:
        app.terminate()
        try:
            app.wait(timeout=8)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    sys.exit(main())
