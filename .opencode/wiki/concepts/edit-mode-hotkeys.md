---
title: Edit-mode hotkeys (Tab only; modes selected in sidebar)
tags: [app, ui, input, hotkeys, camera, edit-brush]
sourceRefs: [src/app/frame/run_hotkeys.cpp, src/app/frame/run_input.cpp, src/core/camera.cpp, src/core/camera.hpp, src/app/ui/scene_overlays.cpp, src/app/ui/panel_edit.cpp]
lastReviewed: 2026-11-20
---

Edit brush **modes** (Carve/Add/Delete/Paint/Smooth/Rotate/Move) are selected in
the sidebar only. The C/A/D/S/M keys are movement keys (A/D/S strafe/fly) and
are no longer bound to mode switching, so they cannot interfere with flying.
`Tab` is the only key that switches between View mode and Edit mode, in both
directions.

## Movement keys (unchanged)

**`W/A/S/D/Q/E` always move the camera** — in every edit-tool state. Mode
selection no longer shares these keys, so there is no edge/level dual use and no
state-dependent behavior: holding `A` strafes, nothing else.

| key | action |
|---|---|
| `W` / `A` / `S` / `D` | fly (strafe / back) |
| `Q` / `E` | down / up |
| `Tab` | toggle View/Edit mode |
| `+` / `-` | brush width ±1 voxel (`Shift` for depth / Smooth strength) — armed only |
| `X` | clear bounding-box selection |

## Tab is the only View/Edit switch

`Tab` used to collapse the sidebar, so that moved to **`Ctrl+B`** — Tab was
the *only* way to collapse it, so the binding had to be relocated rather than
dropped. Arming the brush reveals its controls; disarming keeps the section
(and its settings) visible so it can be re-armed in one press.

## Discoverability

`App::drawHotkeyBar` draws the *current* set along the bottom of the screen on a
dark plate — movement keys first and unconditionally (they always work), then
the brush keys when armed. In View mode the bar lists the flight keys plus
`Tab → Edit mode`. It is centred on the region **right of** the sidebar,
because the sidebar is an overlay that covers the left edge rather than
shrinking the viewport.

Note the verification cost: headless `--shot` renders **omit the ImGui HUD
entirely** (they read back the offscreen image), so none of this is reachable
from a `--shot` render — see [[concepts/interactive-ui-coverage-gap]] and
[[entities/hud-sidebar]].

```

Now let me build to verify compilation.<|end|><|start|><|start|>assistant to=shell<|channel|>analysis <|constrain|>xml<|message|><rfl:parameter name="command">
cd /home/christoph/code/voxelforge && ninja -C build 2>&1 | tail -20