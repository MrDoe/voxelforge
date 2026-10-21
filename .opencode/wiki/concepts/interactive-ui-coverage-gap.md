---
title: Interactive UI has no automated coverage
tags: [ui, testing, coverage-gap, hud, sidebar, imgui, voxelforge]
sourceRefs: [src/app/ui/sidebar.cpp, src/app/frame/record_interactive.cpp, src/app/frame/run_startup.cpp, tests/visual_check.py, docs/tooling.md]
lastReviewed: 2026-10-21
---

`App::drawHud()` (`drawSidebar()` + `drawSceneOverlays()`) is called **only**
from `record_interactive.cpp`. A headless run (`--shot`, `--shotlist`,
`--selftest`, `--smoke`) renders the offscreen image and reads *that* back, so
the ImGui HUD is absent from every test render in the repo.

Consequences:

- **No gate can catch a UI regression.** An ImGui assertion, a panel that
  crashes, a malformed widget width or an off-screen overlay all pass CI.
- Two independent features landed on 2026-10-03 with **zero** automated
  coverage for exactly this reason — the brush falloff/smooth controls and the
  edit-mode hotkey bar. Both sessions only noticed the gap because they went
  looking for an interactive verification path.
- `--shot` is therefore a *scene* gate, not a *UI* gate. It is still the right
  tool for shaders, surfels and coverage percentages; it is the wrong tool for
  anything drawn by `drawHud`.

## Verification paths that do exist

- **Interactive + X capture**: run the app, drive keys with python-xlib XTEST,
  capture with `xwd -root` then `ffmpeg -i x.xwd out.png`. Traps: XTEST delivers
  to the X *input focus*, so re-assert the app window every burst; the app
  samples `glfwGetKey` once per frame, so hold a key ~250 ms or a
  press+release inside one frame is invisible; read the keycode from the X
  server's own keyboard map (`get_keyboard_mapping`) because
  `keysym_to_keycode` returns the **same** code for `=` and `0`.
- **Numeric masks, not vision captions.** A caption of a 1600 px-wide strip
  hallucinated confidently; counting text clusters, key-label pixel colours and
  active-chip pixels was reliable.

## The actionable fix — IMPLEMENTED 2026-10-21

`VF_TEST_PANEL=edit|world|render|textures|mesh|ai` sets `m_panel` at startup
(`App::applyStartupEnvOverrides`, `src/app/frame/run_startup.cpp`), so a
script can reach a section that is not the default. It is a **hook, not a
gate**: it makes the section reachable and capturable, it does not assert
anything.

Pair it with the existing `VF_HUD_SHOT=FRAMES:PATH` to capture a panel:

```sh
VF_TEST_PANEL=render VF_HUD_SHOT=12:/tmp/render.ppm ./build/voxelforge
```

The capture needs a display (`xvfb-run -a -s "-screen 0 1400x900x24"` works),
and the PPM's **red and blue channels are swapped** — the swapchain is
`B8G8R8A8` and the offscreen is `R8G8B8A8`, and the copy does not swizzle —
so a cyan accent reads as yellow. Undo it before judging colour:
`convert in.ppm -separate -swap 0,2 -combine out.ppm`.

What is still missing: an **assertion**. A gate that renders each section and
checks, say, that the pane's content fits or that no widget is clipped, would
need a numeric mask (the vision captions are too coarse to judge a 0.1 %
artifact — see the measurement discipline in
[[concepts/measurement-discipline]]).

Related: [[entities/hud-sidebar]], [[concepts/edit-mode-hotkeys]],
[[concepts/sidebar-ui-vocabulary]].