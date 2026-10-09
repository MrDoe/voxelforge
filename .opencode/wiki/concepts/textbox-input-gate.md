---
title: "Textbox input gate"
tags: [input, imgui, camera, hotkeys, latch, bug-fix]
sourceRefs: [src/app/frame/frame.hpp, src/app/frame/run_input.cpp, src/app/frame/run_hotkeys.cpp, src/app/frame/run.cpp, src/app/app.hpp]
lastReviewed: 2026-10-09
---

**Never move when the cursor is in a textbox.** While any ImGui text field owns
the keyboard, the frame loop must not move the view, fire a hotkey, pick, or
stamp.

## What counts as "in a textbox"

`textFieldOwnsKeyboard()` in `frame/frame.hpp` — one line,
`ImGui::GetIO().WantTextInput`. It is true only while an `InputText` /
`InputTextMultiline` is **active**, which covers, at once:

- the chat composer (`chat_ui.cpp`, multiline),
- the World section's layer filter (`panel_world.cpp`),
- the Mesh section's path / layer name / anchor X,Y,Z / fit / scale / yaw
  (`panel_mesh.cpp`),
- the Render section's sun time field (`panel_render.cpp`).

`DragFloat` / `DragInt` are deliberately NOT included: they are not text fields,
they absorb the mouse, and typed characters route to the active widget and stop
there. The user can still resize the brush and change exposure with `+`/`-`
while hovering a slider.

## Before this change

The gate was `m_chatUi.wantsCaptureKeyboard()` — the chat composer's
`m_inputFocused` — and it only froze the camera. Every other `InputText` in the
app flew the camera and tripped hotkeys while the user typed into it: typing a
layer name or a mesh path moved the view through `handleHotkeys`' whole key
table. It also did not even cover its own menu: with the chat focused, typing
`hat` in the composer toggled SSR (H) and raised exposure (T). And
`m_inputFocused` is only assigned while the AI panel is drawn, so leaving that
section could leave a stale `true` behind and freeze the camera for the rest of
the run — reading `WantTextInput` directly cannot, because ImGui clears it the
frame the widget stops being submitted.

## The gate

One flag, one reading per frame, three consumers (`run.cpp`):

```
fx.textCaptures = updateCamera(dt);          // READS it and freezes the view
processInput(fx.textCaptures, ...);          // gated
handleHotkeys(fx.textCaptures, ...);         // gated
```

- **Camera** — `Camera::update` owns the RMB look, the WASD/QE strafe and the
  scroll speed, so skipping the call freezes the whole view. The mouse delta is
  drained so the first RMB drag after leaving the field does not yank the view
  by however far the cursor travelled while the camera was frozen.
- **Picking/stamps** — `processInput` drains the button latches and returns.
  The click that leaves the field is spent on the field, not on the world
  behind it: with the brush armed, a click in the scene used to carve a voxel in
  the same frame it dismissed the text box. Only `m_lmbWasDown` / `m_ctrlWasDown`
  are kept current, so the frame focus leaves gets a clean edge rather than a
  phantom one.
- **Brush preview** — deliberately survives: it reads `m_latchedHover`, which the
  drain does not clear, so the tint stays on screen while a field is typed into
  (the same reason resizing the brush from the sidebar does not blink it out).

## The one-frame lag, and why the latches must stay warm

The frame loop polls GLFW **before** building the UI (`drawHud` /
`ImGui::NewFrame` run inside `recordInteractiveFrame`, later in the same frame),
so `WantTextInput` reads the **previous** frame's verdict. That is bounded and
harmless on its own: on the frame the click focuses a field the user is holding a
mouse button over the sidebar, not WASD.

It is NOT harmless on the way out. The frame the gate drops is not the frame the
user stopped typing: ImGui consumes the keypress that *moves* focus off the field
(Tab) inside that same `NewFrame`, which clears `WantTextInput` for the next
frame while `glfwGetKey` still reports the key held. A `handleHotkeys` that
skipped its latch while gated would then see `now == true` against a stale
`prev == false` and fire that key once — **measured live**: typing in the layer
filter and pressing Tab armed the edit tool in the same millisecond the field
lost focus.

So while gated, `handleHotkeys` still polls every key latched and discards the
result (`keyEdge` over `GLFW_KEY_SPACE..GLFW_KEY_LAST`). Two traps there:

- **One shared latch table.** The first version put a `static` vector inside
  each of the two lambdas, which is *two* tables — the warm-up wrote the table
  the action loop never read. It now lives in the anonymous namespace as
  `g_keyLatch`, with `kKeyLatchCount` as its size.
- **Bound the scan to real key codes.** Scanning `0..1023` made GLFW invoke its
  error callback 1020 times per typed frame ("Invalid key 1005…"), because the
  app's error callback logs.

## How it was verified

`tools/test_inject.py`'s XTEST harness (there is no automated coverage for the
interactive frame loop — see [[concepts/interactive-ui-coverage-gap]]). A focused
probe proved it with a control per phase, because a frame diff of 0.000 means
nothing unless the same measurement on an unfocused window produces a large
one. Four traps the probe hit first, all of which produced false failures:

1. **Binding to the wrong window.** `--shot` runs open a small, static
   `Voxelforge` window that is not in the pre-launch snapshot, so `VFInput`
   latched onto a peer's render: every input "did nothing" with a diff of
   exactly 0.000. It now requires 1280x720 geometry.
2. **The idle reference was itself post-move.** Comparing a pre-move baseline
   with a post-move frame measured the *camera move* as the noise floor, so the
   control and the idle diff came out identical. The idle shot must be taken
   after the move has already happened.
3. **The frame loop is stalled after the load log.** `layered_world: load`
   precedes the first interactive frame by seconds; input during that window is
   invisible. Wait, and probe delivery with a key that logs (T) before measuring
   anything.
4. **The WM owns focus.** Cinnamon/muffin reverts `set_input_focus` immediately;
   the X focus stayed on Firefox for a whole run, so every injected key went to
   the wrong window. `wmctrl -i -a <win>` is what actually works.

The app logs `keyboard -> text field (input frozen)` / `keyboard -> app` once
per focus transition — the independent proof a field was really focused, which a
frame diff cannot give (the icon rail repaints on hover and reads as "focused").

Final result: 10.5 s of typing into the layer filter, zero hotkey log lines,
scene diff 0.0006, camera moves again on W afterwards.
