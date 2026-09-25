---
title: X11 Input Injection for UI Verification
tags: [x11, xtest, input-injection, testing, python-xlib, gui]
sourceRefs: [tools/vf_input.py, tools/test_inject.py, tools/test_inject_iso.py, tools/diag_xtest.py, tools/diag_windows.py, src/platform/window.hpp, src/app/main.cpp]
lastReviewed: 2026-09-25
---

Headless `--shot` renders the **offscreen** image, so the ImGui HUD is absent
from every test render — no visual gate can catch a GUI regression. To verify
UI or input behaviour you must drive the real window and capture the real X
display. `tools/vf_input.py` does that with python3-xlib (already installed);
`xdotool` and ImageMagick are **not** available.

## How the app reads input

`src/platform/window.hpp` and `src/app/main.cpp` poll state rather than
consuming key events:

```cpp
bool keyPressed(int key) const { return glfwGetKey(m_window, key) == GLFW_PRESS; }
bool mouseDown(int button) const { return glfwGetMouseButton(m_window, button) == GLFW_PRESS; }
```

On X11, `glfwGetKey` resolves through **XQueryKeymap** (the server's keyboard
bit vector), not a callback queue. That is good news: any input that updates
the server keymap is visible to the app, so XTEST is the correct mechanism.
It also means a press+release that both land inside a single frame is
**invisible** — always hold ≥250 ms. Camera look is the exception: it consumes
`glfwGetCursorPos` deltas, so relative pointer warps must generate real
`MotionNotify` events while RMB is held.

## Injection mechanisms

| need | mechanism | notes |
|---|---|---|
| absolute pointer move | `root.warp_pointer(x, y)` | `Window.warp_pointer` targets the window's **parent**; on the root that is the root, so this is absolute screen coords |
| relative pointer move | `display.warp_pointer(dx, dy)` | `Display.warp_pointer(x, y)` is **relative** — do *not* pass a window as the first arg, it lands in the `x` slot and raises `struct.error` |
| window origin in root coords | `root.translate_coords(win, 0, 0)` | the reverse, `win.translate_coords(root, 0, 0)`, returns the **negated** origin |
| key / button events | `xtest.fake_input(d, X.KeyPress, detail=keycode, ...)` | delivered to the X input focus; re-assert focus before each burst |
| capture a frame | `ffmpeg -f x11grab -video_size WxH -i :0+X,Y -frames:v 1 out.png` | the `:0+X,Y` offset must be the window's **root** position |

## Trap 1 — the keymap is bytes, not keycodes

`XQueryKeymap` returns **32 bytes**, where byte N holds keycodes `8N..8N+7`,
LSB first. The naive `km[keycode]` reads a slot that is almost always zero,
which looks exactly like "XTEST keyboard is broken":

```python
km = d.query_keymap()
down = (km[code // 8] >> (code % 8)) & 1     # correct
down = km[code]                              # wrong: always 0
```

A correct probe (`tools/diag_xtest.py`) shows the keymap changing on every
press: **XTEST keyboard injection does work.**

## Trap 2 — a set keymap bit does not mean the app saw it

This is the subtle one. `glfwGetKey` / `glfwGetMouseButton` read **GLFW's
internal** key/button state, which is updated only by KeyPress/ButtonPress
*events* delivered to the X input-focus window. The keymap is global, so it
can read "pressed" while the event went to some other client — the camera
then does not move even though the probe says the key is down.

So: re-assert `set_input_focus(win.id, ...)` immediately before **every**
XTEST burst, and verify `get_input_focus()` is still the target while the key
is held. A burst sent while focus had drifted produces a keymap bit, zero
pixel change, and looks identical to "input is broken".

Related: `glfwGetMouseButton` only reports PRESS while the pointer is **inside
the client rect**. Warp into the window before an RMB-look or it is a silent
no-op, even with focus correct.

**This was the last thing that had to be fixed.** With focus re-asserted
immediately before each burst, the gate is green:

```
window 0x4a0000b (32, 64, 1280, 720)   idle noise 0.003
focus before press: 0x4a0000b  (ours)
W keymap: before=0 during=1; focus ours=True
W-forward diff 0.723   look diff 0.556   (threshold 0.200)
```

## Trap 3 — this server offsets button-mask bits by 8

`XQueryPointer`'s mask does not follow the usual layout here:

| XTEST `detail` | button | mask bit | (standard X) |
|---|---|---|---|
| 1 | LMB | 8 | 0 |
| 2 | MMB | 9 | 1 |
| 3 | RMB | 10 | 2 |
| 4 | — | 11 | 3 |
| 5 | — | 12 | 4 |

So `mask & (1 << 2)` for RMB always reads False on this machine. Never verify
button injection with the mask bit. The XTEST event `detail` is still the
correct button number and is what GLFW maps to `MOUSE_BUTTON_RIGHT`.

## Trap 4 — window lookup by title is ambiguous

Several windows can be titled `Voxelforge` at once: crashed runs leave stale
windows, X recycles window ids, and concurrent test scripts each spawn their
own at different sizes. Binding to the wrong window captures the wrong pixels,
so every A/B diff reads ~0.000 and the whole thing looks like dead input.

Two rules:

1. **Snapshot before launching.** Record the ids of same-titled windows, then
   pass them as `exclude` so the new window is selected.
2. **Select by size, not DFS order.** `hits[0]` routinely picks a leftover
   80x45 or 480x270 window. Prefer the candidate whose geometry equals the
   requested `--width/--height`, else the largest.

```python
pre = VFInput.snapshot_titles()          # before launch
app = subprocess.Popen([...])
cands = [h for h in find_windows(root, TITLE) if h[0].id not in pre]
exact = [h for h in cands if (h[1][2], h[1][3]) == want]
win, geom, _pid = (exact or sorted(cands, key=lambda h: -h[1][2] * h[1][3]))[0]
```

`_NET_WM_PID` is **not** a usable fallback for *finding* the window — the X
server reports PIDs in a different namespace than the shell (observed: X said
86, shell said 253301). It *is* useful the other way round: to tell a live
window from an orphan whose client has exited.

## Trap 5 — offscreen renders still own a titled window

`--shot`, `--shotlist` and `--smoke` create a real GLFW window named
`Voxelforge` even though they render offscreen and write to a file. They
therefore collide with live capture exactly like an interactive run. A
**window-based** guard is required; a process-based one is both too strict
(it blocks on harmless renders) and, if it only checked for the window you
launched, too weak.

## Trap 6 — don't measure with ffmpeg `blackframe`

Parsing `blend=difference,blackframe=...` stderr returned **identical numbers
for every case** (0.090, 0.090, 0.050, 0.050), which masked the real problem
and looked like a coherent-but-wrong result. Use numpy directly:

```python
a = np.asarray(Image.open(p).convert("RGB"), dtype=np.int16)
frac = float((np.abs(a - b).max(axis=2) > 12).mean())
```

Always measure the **idle noise** first (two captures, no input) and set the
pass threshold above it — animated water alone produced 0.14 on a
mis-captured window and 0.002 on a correct one.

## Trap 7 — the scene must finish loading first

The GLFW window exists long before the world is ready; load is ~17.6 s
(see [[concepts/load-time-field-build]]). Capturing too early yields a
near-black frame (luma ~10) and every later comparison looks like a huge
change. Gate on the app's own `layered_world: load` log line, then poll until
consecutive frames agree in mean luma.

## Tools

- `tools/vf_input.py` — the library. `tap`/`hold`/`combo` for keys,
  `look_begin`/`look`/`look_end` for RMB look, `click`, `warp_to`/`center`,
  `snapshot_titles` for safe window binding.
- `tools/test_inject_iso.py` — the authoritative gate. Refuses to run if a
  live `Voxelforge` window exists, selects the new window **by size**, waits
  for the world load, then proves keyboard and mouse-look by A/B frame diffs
  against a measured idle-noise floor.
- `tools/test_inject.py` — the fuller matrix (adds the Edit-panel check).
- `tools/diag_xtest.py` — proves whether XTEST key injection reaches the
  server keymap, with correct byte indexing.
- `tools/diag_windows.py` — lists every `Voxelforge` window with size,
  `WM_STATE` and `_NET_WM_PID`; the first tool to run when a capture looks
  wrong.
- `tools/diag_focus.py` — measures whether the pointer lands inside the
  window and whether the WM holds input focus.
