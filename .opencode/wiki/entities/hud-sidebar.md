---
title: HUD sidebar (the single editor panel)
tags: [ui, imgui, hud, sidebar, ux]
sourceRefs: [src/app/main.cpp, src/app/chat_ui.cpp, src/app/chat_ui.hpp, AGENTS.md, docs/rendering.md]
lastReviewed: 2026-09-25
---

# HUD sidebar — the single editor panel

The editor has **exactly one ImGui window**: `Voxelforge##Sidebar`, a docked
left sidebar. It replaced five floating windows (Dashboard, World Layers,
Toolbox, Materials, Import STL/OBJ) plus the bottom-right AI Assistant.

```
┌──┬────────────────────┐
│ED│  one section only, │   ← rail 46 px, content pane ≈300 px
│WL│  scrolls           │
│RN│                    │
│TX│                    │
│IM│                    │
│AI│                    │
│  ├────────────────────┤
│  │ status footer 52px │
└──┴────────────────────┘
```

## Structure

`drawHud()` is now a two-liner: `drawSidebar()` + `drawSceneOverlays()`.

- **Rail** — six 2-letter buttons. The default ImGui font (ProggyClean) has no
  icon glyphs, so glyph-based icons would render as `?`; 2-letter ASCII labels
  are the deliberate choice. `Ctrl+1..6` are the matching shortcuts.
- **Content pane** — exactly one `Panel` section, chosen by the rail.
- **Footer** — fixed 52 px, two status rows, so switching sections never
  reflows it. A staged rotation/move takes over the whole footer from *any*
  section: it is a pending `world.json` write, and burying the Apply button
  inside the Edit section made it easy to lose.
- **Collapsed** — `Ctrl+B` shrinks the sidebar to the rail alone. (It was `Tab`
  until the edit-mode shortcuts landed; `Tab` now switches View/Edit mode.)
  The footer degrades to a single frame-time figure because there are only
  ~30 px of content width left.
- **Horizontal resize** — an 8 px grip at the right edge changes only the
  sidebar width. The left/top/bottom edges stay pinned, the width persists
  for the session while switching sections or collapsing/re-expanding, and
  display changes clamp it to `rail + 150 px` through `display - 8 px`.
  It is deliberately not written to `imgui.ini`. Double-clicking the grip
  restores the responsive default (`kRailW + min(300 px, 30% viewport)`).

## State model

`enum class Panel { Edit, World, Render, Textures, Mesh, AI }` + `m_panel`
replaced three window-visibility booleans (`m_showWorldLayers`,
`m_showMeshImport`, `m_showTextures`) and `ChatUi::m_visible`.

`m_editActive` is **not** part of that. It is tool-armed state: it gates LMB
stamping (`editLmb`) and hover computation in the frame loop, so it must
survive a UI refactor. `C` toggles it *and* switches the rail to Edit; an accent
bar on the `ED` rail button marks the armed state from any section.

`ChatUi::draw` became `ChatUi::drawPanel`: no `Begin`/`End`, no positioning, no
visibility flag. Request polling already ran unconditionally above the old
`if (!m_visible) return;`, so a response that lands while another section is
showing still reaches `m_history`.

## Invariants

- **It is an overlay.** The render stays full-window and the pick ray still runs
  camera→cursor, so a wider sidebar costs screen area only — no change to
  aspect, offscreen size, or `projectScreen`.
- **Idle means fully docked.** The window is forced to `(0,0)–(width,height)`,
  has square/no-border/no-title decoration (including zero child rounding and
  borders), uses alpha `1.0`, and gives child surfaces the same background
  colour. This removes both scene bleed and the
  darker top/bottom padding bands that made the panel look inset. During a
  trackball drag only the parent fades to `0.30`; child backgrounds become
  transparent so the alpha is not applied twice.
- **Resize owns scene input.** The frame loop hit-tests the same 8 px grip
  before ray picking/stamping and suppresses trackball/move-handle hits. A drag
  therefore changes layout without selecting, painting, moving, or rotating.
- **`NoSavedSettings` + `ImGuiCond_Always`.** A `imgui.ini` saved by the old
  floating layout can never displace the sidebar. This is why the chat's
  position self-heal hack could simply be deleted.
- **Ring guard.** `hoveredTrackballHandle` is computed before ImGui mouse
  capture; while the pointer is on a ring the whole sidebar gets
  `ImGuiWindowFlags_NoInputs`, and an active drag fades it to 0.30 alpha.
  See [[concepts/layer-placement]].
- **Width discipline.** The pane is ≈300 px, ≈256 px at 960×540. Use
  `-1.0f` item widths; hard-coded widths are how the old 3-column Materials and
  2-column Transform tables had to become stacked layouts.
- **`Tab` and `Ctrl+B` are gated on `!WantTextInput`** so the chat's multiline
  input keeps them as normal characters.

## The `edge()` latch

`edge(k)` is a per-key-code state latch, not a consume — whichever block polls
a key first in a frame takes the edge. `Ctrl+1..5` (sidebar sections) and bare
`1..5` (render flags) share key codes, so the section loop polls first *and* is
guarded by `if (ctrl)`: it claims the edge on exactly the frames the bare loop
must ignore. Adding a `!ctrl` guard to the render loop instead would leave its
latch stale and fire a spurious toggle when Ctrl was released.

## Verifying a UI change here

Headless `--shot` reads back the **offscreen** image, so the HUD is absent from
every test render — the visual gates cannot catch a UI regression. Verify a
sidebar change by running the app and looking:

- Ground truth for "no floating windows": temporarily log
  `ImGui::GetCurrentContext()->Windows` (needs `imgui_internal.h`) and check
  every entry is `Voxelforge##Sidebar` or a `/`-child of it. At 1600×900 the
  expected set is Sidebar(346×880) + rail(46) + pane(268) + any table children.
- Check 960×540 as well — that resolution is where the old clipped
  `BrushModes` table segfaulted. The responsive width is 334 px there and
  346 px at 1600×900.
- With XTEST, press at the grip (`x = default width`, mid-height), drag
  horizontally, and release. The log should report `sidebar width -> N px`, the
  scene must begin at the new opaque boundary, and the grip must remain inside
  that boundary. Double-click the grip to recover the default.
- Drive keys with XTEST. **Hold the key ≥ ~250 ms**: the app samples
  `glfwGetKey` per frame, so a press+release that both land inside one frame
  is invisible. See [[concepts/focused-test-groups]] for what the gates do and
  do not cover.
