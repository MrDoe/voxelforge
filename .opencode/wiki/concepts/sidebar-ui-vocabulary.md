---
title: The sidebar's UI vocabulary (tokens + widgets)
tags: [ui, imgui, hud, sidebar, design-tokens, ux]
sourceRefs: [src/app/ui/ui_types.hpp, src/app/ui/ui_primitives.hpp, src/app/ui/ui_primitives.cpp, src/app/ui/theme.cpp, src/app/ui/sidebar.cpp, docs/tooling.md]
lastReviewed: 2026-10-21
---

# The sidebar's UI vocabulary

The 2026-10-21 rework replaced the sidebar's ad-hoc ImGui calls with a small
**design system**: tokens in `ui/ui_types.hpp`, widgets in
`ui/ui_primitives.{hpp,cpp}`, palette in `ui/theme.cpp`. New UI is built from
those, not from raw `ImGui::Text` calls.

## Why it exists

The old sidebar was a flat list. Measured on the 1280×720 capture (before →
after), the concrete defects were:

- The **section title was the loudest thing on screen** (accent-coloured, all
  caps) while the controls were the same size and colour as the prose around
  them — no hierarchy above "one flat list".
- Two lines were **clipped by the pane's right edge**: the section's
  description (`TextDisabled` does not wrap) and the layer count row
  (`"24 files | 15 enabled | 4740930 records live"`).
- The **rail was a code to decode**: `ED / WL / RN / TX / IM / AI`.
- The **footer was a run-on line** (`"splat 36.0 ms"` / `"56 CPU | geo 9.6"`)
  that had to be parsed rather than read.
- The **empty inspector** was a blank half-panel.

## The rules

1. **The accent means STATE, never a label.** Cyan marks what is active,
   armed, on or staged. A page where everything is highlighted highlights
   nothing. (`kAccent` in `ui_types.hpp`.)
2. **Four surface levels**, so a control is distinguishable from the panel it
   sits in without borders doing all the work: window < raised (group band) <
   control (frame) < overlay (popup).
3. **Three text levels** — primary (values), secondary (hints, group titles),
   tertiary (units, disabled). The step between them is what makes a flat list
   read as structure.
4. **One rhythm.** Every gap, row and band is a multiple of `kUiGap`; the
   metrics live in `ui_types.hpp` so the frame loop and the panels agree.
5. **Readouts are DATA**: `keyValue(label, fmt, ...)` puts the label left and
   the value right, so numbers line up in a column and can be compared between
   frames. Prose is for explanations (`hintLine`), not for numbers.
6. **A group is collapsible and says what it holds**: `groupHeader(id, title,
   defaultOpen, badge)` draws a band with a chevron, and the badge (right
   aligned) keeps a *closed* group informative — the Textures panel uses it to
   show the current file per material.

## The widget vocabulary

| Widget | Use it for |
|---|---|
| `paneBand(title, shortcut)` | the pane header: section name + accent tab + `Ctrl+N` chip |
| `sectionHeader(hint)` | the one muted line under the band (it WRAPS — the old one did not) |
| `groupHeader(id, title, defaultOpen, badge)` | a collapsible section; `id` is the persistent state key |
| `groupEnd()` | the hairline + air that closes a group |
| `keyValue(label, fmt, ...)` / `keyValueColored(...)` | a readout row |
| `hintLine(fmt, ...)` | the wrapped note under a control |
| `statusPill(text, color)` | a state word (LIVE / OFF / STAGED) |
| `actionButton(label, active, size)` | a toggle that reads as "currently on" |
| `drawPanelIcon(panel, centre, size, colour)` | the rail's vector glyphs |

## The rail's icons

The default ImGui font (ProggyClean) has no icon glyphs, which is why the
rail used two-letter codes. The icons are now **drawn with `ImDrawList`** from
primitives, so they are themeable and sharp at any size:

| Section | Glyph |
|---|---|
| Edit | a chisel (triangle tip + body quad + ferrule) |
| World | three stacked rhombi (layers) |
| Render | a sun disc with eight rays |
| Textures | a 2×2 checker swatch |
| Mesh | an isometric cube |
| AI | a chat bubble with a tail |

The active section is marked three ways (filled pill, accent glyph, accent
underline) because the rail is the one thing that must stay legible while the
eye is on the 3D scene. The **armed brush is a separate dot** on the Edit
glyph: the brush is armed independently of which section is open.

## The wheel belongs to the UI

The sidebar's pane scrolls with the wheel. It used to carry
`NoScrollWithMouse`, and the reason given was that the wheel drove the
camera's movement speed.

**That binding was invisible and is gone.** `Camera::update` did
`speed *= exp(-scroll * 0.15)` from the platform window's own scroll
accumulator — and ImGui's GLFW backend *chains* the app's scroll callback, so
a scroll over the sidebar changed how fast the camera flew, with no readout
anywhere saying so. The speed is now an explicit setting (the Render panel's
VIEW group), and the app installs no scroll callback at all.

The trap worth remembering: `grep io.MouseWheel` finds nothing, because the
app did not use ImGui's wheel - it used `Window::scrollDelta()`. **A grep of
one input path is not a proof that an input is unbound**; follow the binding
to its consumer.

## Verifying a UI change

The sidebar is drawn by the **interactive path only**, so no headless
`--shot` render contains it. Two hooks make it checkable:

- `VF_TEST_PANEL=edit|world|render|textures|mesh|ai` opens a section at
  startup (added 2026-10-21, closing the actionable part of
  [[concepts/interactive-ui-coverage-gap]]).
- `VF_HUD_SHOT=FRAMES:PATH` copies the composed swapchain frame (HUD included)
  to a PPM after N presented frames.

Together: `VF_TEST_PANEL=render VF_HUD_SHOT=12:/tmp/render.ppm ./build/voxelforge`.

**The PPM's red and blue channels are swapped** (the swapchain is
`B8G8R8A8`, the offscreen is `R8G8B8A8`, and the copy does not swizzle), so
a cyan UI reads as yellow in the capture. Undo it before judging colour:
`convert in.ppm -separate -swap 0,2 -combine out.ppm`.

### The loop that found the defects

Capture each section, crop the pane, and **read the pixels** — a caption is
too coarse to see a label that is cut in half. The first pass found, in this
order: the clipped section description, the clipped layer-count row, the
clipped widget labels (13 of them), the badge colliding with its title, the
chat's status line running past the pane, the chat's button row behind the
status bar, and the Mesh transform labels clipped by the label column.

Check **960×540** as well: the pane is ~256 px of content there, so the
label column is ~102 px, and that is the resolution where the old clipped
`BrushModes` table segfaulted. Both sections run clean at that size.

### The model that reads the capture

`opencode-rag.json` points `imageDescription` (and its `onDemand` override) at
**`minimax/minimax-m3.1-flash:free`** on OpenRouter, so `describe_image` is a
MiniMax vision pass, not the local qwen3-vl-8b. Two things to know: the tool
snapshots its config at plugin-init (an edited config needs a plugin reload),
and MiniMax is a *reasoning* model — a short visual question can come back
with the answer in `reasoning` rather than `content`. It is accurate about
layout and text, and good enough to spot a label cut in half, which is
exactly the class of defect this loop hunts.

See [[entities/hud-sidebar]] for the sidebar's structure and invariants.
