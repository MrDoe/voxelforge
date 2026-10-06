---
title: Interactive UI has no automated coverage
tags: [ui, testing, coverage-gap, hud, sidebar, imgui, voxelforge]
sourceRefs: [src/app/ui/sidebar.cpp, src/app/frame/record_interactive.cpp, tests/visual_check.py]
lastReviewed: 2026-10-03
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

## The actionable fix (not implemented)

A `VF_TEST_PANEL=edit` hook that sets `m_panel` and lets the shot path call
`drawHud()` **once**. That would make ImGui's own assertions fire headlessly and
turn a coverage gap into a test — the sidebar already runs `BeginTable()` /
`BeginChild()` returns that are individually guarded, so the machinery is
there. Recorded with the hook name so it is a task rather than a regret.

Related: [[entities/hud-sidebar]], [[concepts/edit-mode-hotkeys]].