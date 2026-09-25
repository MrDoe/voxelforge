#!/usr/bin/env python3
"""vf_input.py - drive a running Voxelforge window from a script.

Uses the XTEST extension for keyboard / mouse-button events and
XWarpPointer for pointer motion. GLFW's X11 backend observes both through
the normal event path, so injected input is indistinguishable from real
hardware input as far as the engine is concerned:

  * Camera::applyLook consumes pointer deltas from glfwGetCursorPos, and
    every XWarpPointer produces a MotionNotify that updates that position.
  * Camera::update samples glfwGetKey / glfwGetMouseButton state, which
    XTEST KeyPress / ButtonPress events toggle.

Movement in the app is dt-integrated, so holding a key for a wall-clock
duration yields a framerate-independent displacement - the timeline can be
authored in seconds rather than frames.

Usage:
    inp = VFInput()                 # finds the "Voxelforge" window
    inp.tap('c')                    # toggle the edit tool
    inp.hold('w', 3.0)              # fly forward 3 s
    inp.look_begin(); inp.look(400, 0, 2.0); inp.look_end()
"""

import time

from Xlib import X, display
from Xlib.ext import xtest
from Xlib.xobject.drawable import Window

# X keysyms -> logical key names used by the timeline.
KEYSYMS = {
    "w": 0x0077, "a": 0x0061, "s": 0x0073, "d": 0x0064,
    "q": 0x0071, "e": 0x0065, "c": 0x0063, "f": 0x0066,
    "n": 0x006E, "g": 0x0067, "h": 0x0068, "j": 0x006A,
    "k": 0x006B, "l": 0x006C, "m": 0x006D, "t": 0x0074,
    "b": 0x0062, "z": 0x007A,
    "shift": 0xFFE1,   # Shift_L
    "ctrl": 0xFFE3,    # Control_L
    "esc": 0xFF1B,
    "space": 0x0020,
    "lb": 0x005B,      # [
    "rb": 0x005D,      # ]
    "equal": 0x003D,   # = / +
    "minus": 0x002D,   # - / _
}

LMB, RMB = 1, 3
LOOK_SENS = 0.0022  # rad per px, must match Camera::kSensitivity


class VFInput:
    """X11 input injection into a running Voxelforge window."""

    def __init__(self, dpy_name=":0", title="Voxelforge", exclude=()):
        self.d = display.Display(dpy_name)
        self.root = self.d.screen().root
        self.codes = {}
        for name, sym in KEYSYMS.items():
            code = self.d.keysym_to_keycode(sym)
            if code == 0:
                raise SystemExit(f"vf_input: no keycode for key {name!r}")
            self.codes[name] = code
        self.win: Window = self._find_window(title, exclude)
        self.focus()
        # virtual pointer position, tracked so sweeps can be planned in px
        self.px, self.py = self.query_pointer()
        self._rmb = False

    # ---------------------------------------------------------------- window

    def _find_window(self, title, exclude=()) -> Window:
        """Depth-first search for a client window by WM_NAME.

        `exclude` holds window ids seen before the app was launched. A
        crashed run can leave a stale same-titled window behind, and X reuses
        ids, so a plain title match can latch onto the wrong window and
        silently capture the wrong pixels. Preferring ids that are *not* in
        the pre-launch snapshot is what makes this reliable.
        """
        stack = [self.root]
        fallback = None
        while stack:
            w = stack.pop()
            try:
                if w.get_wm_name() == title:
                    if w.id not in exclude:
                        return w
                    fallback = fallback or w
                stack.extend(w.query_tree().children)
            except Exception:
                # withdrawn / foreign windows may not answer
                pass
        if fallback is not None:
            return fallback
        raise SystemExit(
            f"vf_input: window {title!r} not found - is the app running?")

    @classmethod
    def snapshot_titles(cls, dpy_name=":0", title="Voxelforge"):
        """Ids of currently-open windows with this WM_NAME.

        Call before launching the app, then pass the result to VFInput so it
        binds to the freshly created window rather than a stale leftover.
        """
        d = display.Display(dpy_name)
        root = d.screen().root
        found = set()
        stack = [root]
        while stack:
            w = stack.pop()
            try:
                if w.get_wm_name() == title:
                    found.add(w.id)
                stack.extend(w.query_tree().children)
            except Exception:
                pass
        return found

    def focus(self):
        """Keyboard focus + raise, so XTEST key events are delivered here."""
        try:
            self.win.configure(stack_mode=X.Above)
        except Exception:
            pass
        try:
            self.d.set_input_focus(self.win.id, X.RevertToParent, X.CurrentTime)
        except Exception:
            pass
        self.d.flush()

    def geometry(self):
        """Client rect in root coords: (x, y, width, height)."""
        g = self.win.get_geometry()
        tr = self.root.translate_coords(self.win, 0, 0)
        return (tr.x, tr.y, g.width, g.height)

    # -------------------------------------------------------------- keyboard

    def _send_key(self, name, press):
        code = self.codes[name]
        ev = X.KeyPress if press else X.KeyRelease
        xtest.fake_input(self.d, ev, detail=code, time=X.CurrentTime)
        self.d.flush()

    def key(self, name, press):
        self._send_key(name, bool(press))

    def tap(self, name, hold=0.07, settle=0.06):
        self._send_key(name, True)
        time.sleep(hold)
        self._send_key(name, False)
        time.sleep(settle)

    def combo(self, *names, hold=0.07, settle=0.06):
        """Press modifiers, tap the final key, then release modifiers."""
        if not names:
            raise ValueError("combo needs at least one key")
        key, mods = names[len(names) - 1], list(names[:-1])
        for m in mods:
            self._send_key(m, True)
        self._send_key(key, True)
        time.sleep(hold)
        self._send_key(key, False)
        for m in reversed(mods):
            self._send_key(m, False)
        time.sleep(settle)

    def hold(self, name, dur, settle=0.04):
        self._send_key(name, True)
        time.sleep(dur)
        self._send_key(name, False)
        time.sleep(settle)

    # --------------------------------------------------------------- pointer

    def _send_button(self, button, press):
        ev = X.ButtonPress if press else X.ButtonRelease
        xtest.fake_input(self.d, ev, detail=button, time=X.CurrentTime)
        self.d.flush()

    def query_pointer(self):
        q = self.root.query_pointer()
        return q.root_x, q.root_y

    def warp(self, dx, dy):
        """Relative pointer move; generates a real MotionNotify."""
        self.px += dx
        self.py += dy
        self.d.warp_pointer(dx, dy)
        self.d.flush()

    def warp_to(self, x, y):
        """Absolute pointer move to root coords."""
        self.px, self.py = x, y
        # Window.warp_pointer targets self's parent; on the root window that
        # is the root itself, so this lands at absolute screen coords.
        self.root.warp_pointer(x, y)
        self.d.flush()

    def center(self):
        x, y, w, h = self.geometry()
        self.warp_to(x + w // 2, y + h // 2)

    # ----------------------------------------------------------- mouse-look

    def look_begin(self, settle=0.05):
        """Hold RMB so pointer motion feeds Camera::applyLook."""
        if not self._rmb:
            self._send_button(RMB, True)
            self._rmb = True
        time.sleep(settle)

    def look_end(self, settle=0.05):
        if self._rmb:
            self._send_button(RMB, False)
            self._rmb = False
        time.sleep(settle)

    def look(self, dx_px, dy_px, dur, rate_hz=60):
        """Smooth pointer sweep over `dur` seconds while RMB is held.

        The accumulated pixel delta is exact regardless of the sweep rate,
        because Camera::applyLook integrates pointer displacement.
        """
        steps = max(1, int(round(dur * rate_hz)))
        per = 1.0 / rate_hz
        sx, sy = dx_px / steps, dy_px / steps
        for _ in range(steps):
            self.warp(int(round(sx)), int(round(sy)))
            time.sleep(per)

    def look_to(self, d_yaw, d_pitch, dur, rate_hz=60):
        """Sweep by an angle (radians): +yaw turns view right, +pitch up."""
        self.look(-d_yaw / LOOK_SENS, -d_pitch / LOOK_SENS, dur, rate_hz)

    def recenter(self, x, y, settle=0.07):
        """Warp the pointer back with RMB released (delta consumed, ignored).

        Camera::update calls getMouseDelta every frame but only applies it
        while RMB is down, so a recenter between look blocks is invisible.
        """
        self.look_end()
        self.warp_to(x, y)
        self.look_begin(settle)

    # ----------------------------------------------------------------- misc

    def click(self, button=LMB, hold=0.08, settle=0.10):
        self._send_button(button, True)
        time.sleep(hold)
        self._send_button(button, False)
        time.sleep(settle)

    def sync(self, secs):
        time.sleep(secs)
