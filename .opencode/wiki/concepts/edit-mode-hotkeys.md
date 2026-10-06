---
title: Edit-mode hotkeys and the dual-purpose WASD keys
tags: [app, ui, input, hotkeys, camera, edit-brush]
sourceRefs: [src/app/frame/run_hotkeys.cpp, src/app/frame/run_input.cpp, src/core/camera.cpp, src/core/camera.hpp, src/app/ui/scene_overlays.cpp, src/app/ui/panel_edit.cpp]
lastReviewed: 2026-10-04
---

The brush mode shortcuts (`C` Carve, `A` Add, `D` Delete, `S` Smooth, `M` Move)
took over three keys that were camera keys forever. That makes the keyboard
**state-dependent**, which is the whole design constraint. The state machine is
strict: **`Tab` is the only key that switches between View mode and Edit
mode**, in both directions. A mode key pressed in View mode does nothing (it
cannot drag you into Edit), and no non-Tab key leaves Edit mode either.

## Routing

**`W/A/S/D/Q/E` always move the camera** — in every edit-tool state. While the
brush is armed, A/D/S are dual-purpose: the brush mode is picked on the
**edge** (one action per press) while flying is **level-triggered** (continuous
while held), so holding `A` strafes and selects Add once. The two uses do not
conflict.

| key | press (edge) | hold (level) |
|---|---|---|
| `W` / `Q` / `E` | — | fly |
| `A` / `D` / `S` | Add / Delete / Smooth — **Edit mode only** | fly (strafe / back) |
| `C` | Carve — **Edit mode only** | — |
| `M` | Move — **Edit mode only** | — |
| `Tab` | toggle View/Edit mode | — |

### Reversed decision: do NOT yield the movement keys to the brush

An earlier version made the camera yield A/D/S while the brush was armed
(`Camera::moveKeysYieldsToBrush`, set every frame in `updateCamera`, with a
duplicated edge-latch to avoid a one-frame camera nudge because `updateCamera`
runs *before* `handleHotkeys`). **That was reverted**: flying is how you aim the
brush, so taking movement away while editing was the wrong trade. The mechanism
and its latch are gone rather than left disabled — a disabled flag named
"yields" is a trap for the next reader.

### Reversed decision: mode keys do NOT arm the brush

An earlier version let the mode keys arm the brush when disarmed ("one keypress
leaves the tool in the state you asked for") and let `C` disarm on a second
press. That was wrong in practice: in View mode a stray `A`/`D`/`S` while
flying — or `C`/`M` — switched to Edit mode, exactly the mode the user was
*not* asking for. Now the mode keys act only while armed and `C` never
disarms; **only `Tab` switches the mode**. Do not restore the old behavior.

`Tab` used to collapse the sidebar, so that moved to **`Ctrl+B`** — Tab was
the *only* way to collapse it, so the binding had to be relocated rather than
dropped.

## Traps hit while building this

1. **The `edge()` latch is stateful — read every mode key every frame.** Two
   ways to get this wrong: calling `edge(GLFW_KEY_C, …)` twice for one key
   makes the second call return false *forever* (read it once into a bool);
   and short-circuiting a key while disarmed (bailing before the reads) leaves
   its latch stale, so it phantom-fires on the frame the brush arms. All five
   edges are read unconditionally, then acted on only while armed.

2. **Mode selection must go through `App::chooseEditMode`**, the single
   implementation shared with the sidebar's mode buttons. It refuses a switch
   that would orphan a staged rotate/move or race a committing preview; a bare
   `m_editBrush = …` from the hotkeys would silently discard state the panel
   would have protected.

3. **Ordering: `updateCamera` runs BEFORE `handleHotkeys`.** This only bit the
   reverted yield design (needing a duplicated latch to avoid a one-frame
   nudge); with the yield gone it is a non-issue, but it is the reason that
   design was fiddly and is worth remembering before writing any future
   key-gating that spans both functions.

A pure helper (`Camera::movementForBrush`) existed only to unit-test the yield;
it was deleted with it. The current test asserts the property that actually
matters — the six movement keys are live regardless of edit-tool state — using
the long-standing pure `Camera::computeMove`.

`+`/`-` already existed (width ±1 voxel, `Shift` for depth / Smooth strength)
and act only while armed; disarmed they are exposure.

## Discoverability

`App::drawHotkeyBar` draws the *current* set along the bottom of the screen on a
dark plate — movement keys first and unconditionally (they always work), then
the mode keys when armed. In View mode the bar lists the flight keys plus
`Tab → Edit mode`; the mode keys are deliberately absent there, because
advertising them would promise a switch that no longer happens. It is centred
on the region **right of** the sidebar, because the sidebar is an overlay that
covers the left edge rather than shrinking the viewport.

Note the verification cost: headless `--shot` renders **omit the ImGui HUD
entirely** (they read back the offscreen image), so none of this is reachable
from a `--shot` render — see [[concepts/interactive-ui-coverage-gap]] and
[[entities/hud-sidebar]].
