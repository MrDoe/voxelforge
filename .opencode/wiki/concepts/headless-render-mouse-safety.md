---
title: Headless renders must never steal the user's mouse or focus
tags: [gotcha, testing, xvfb, glfw, workflow]
sourceRefs: [src/platform/window.cpp, src/app/frame/run_startup.cpp, tests/visual_check.py]
lastReviewed: 2026-10-21
---

# Rendering for measurement without disturbing the user

The user asked for this directly: **"Don't grab the mouse when testing!"**
It is a workflow rule, not a rendering bug.

## The two ways a test run can take the mouse

1. **The interactive app.** `./build/voxelforge` opens a real window and
   grabs the cursor on right-mouse look (`src/platform/window.cpp`:
   `glfwSetInputMode(..., GLFW_CURSOR_DISABLED)`). Launching it while the
   user is working hands them a window that follows the pointer, and a
   stray right-click *captures* the mouse for good.
   **Do not launch the interactive app from a session unless the user asks
   for it.**
2. **A headless `--shot` still creates a GLFW window.** It is not mapped
   visibly any more (2026-10-21: the headless path creates the window
   unmapped, so it cannot take focus), but an *old* binary still maps it -
   and a mapped window on the user's display is enough to make the pointer
   look stuck.

## The safe recipe

```bash
# every scratch render: a virtual display, so no window ever appears on the
# user's screen and nothing can steal focus or the pointer
VF_OVERLAY_PATH=/tmp/opencode/ada/overlay-copy.vxw \
  xvfb-run -a ./build/voxelforge --shot /tmp/out.ppm \
  --cam -4.2,0.9,14.6,-5.404,-0.488,4.792 --width 1280 --height 800
```

- `xvfb-run -a` needs a working `Xvfb` (present on this box). It gives each
  run its own display, so parallel renders do not fight over one.
- `VF_OVERLAY_PATH=<tmp>` keeps the run off `assets/runtime_edits.vxw`: a
  headless run is **not** read-only against the tracked overlay (the app
  loads it and writes it back). `VF_NO_OVERLAY=1` suppresses only the
  *load*, and a run that also stamps still writes.
- Reference shots (the test suites) use `VF_NO_OVERLAY=1` so a session's
  painting cannot pollute them.

## Why it matters beyond politeness

A render that opens a window on the user's display is also a *focus* event:
their next click lands in my app instead of theirs, which is how a
measurement session ends up editing the user's scene. `xvfb-run` removes
the whole class of accidents.

## See also

- [[concepts/splat-canopy-parity]] - the measurement rig that needs this
- [[concepts/focused-test-groups]] - the group-only test convention
