---
title: Brush preview visibility — the tint can only mark existing surfels
tags: [live-edit, brush, preview, splat, svo, post, ui, measurement, gotcha]
sourceRefs: [src/app/frame/run_brush_preview.cpp, src/app/frame/run_input.cpp, src/render/svo_pass.cpp, src/render/post_pass.cpp, shaders/splat.frag, shaders/svo_raymarch.comp, shaders/post.comp, shaders/common_surfel.glsl, tests/live_edit_check.py]
lastReviewed: 2026-10-01
---

# Brush preview visibility

The hover preview is how the user sees a brush's size before clicking. It is
**not** a drawing of the brush volume — it is a recolour of the *existing
surfels* whose centre falls inside the CPU volume. That single design choice
is the root of three separate "the brush size/depth does not show up"
symptoms, all found by measurement rather than reading. See
[[entities/live-edit-brush]] for the modes and [[concepts/measurement-discipline]]
for the noise-floor discipline this needs.

## The invariant

`inBrushVolume()` can only mark geometry that is **already in the scene**.

| brush axis | where the volume goes | can the tint show it? |
|---|---|---|
| Width (radius) | across the surface, where surfels exist | **yes** — measured 68–72 % pixel diff, 0.1→12 m |
| Carve depth | *below* the surface, into solid material | **no** — no surfels down there |
| Add depth | *above* the surface, into empty air | **no** — the growth has no splats yet |

Measured on a flat cell, debug view 15 (magenta = affected surfel):
Carve's mask was **bit-identical at 2920 px** for depths 0.1 / 0.5 / 2.0 /
12.0 m, and Add's saturated at 2790 px for depth ≥ 0.5 m. The Depth slider
was *completely* inert on screen.

## Why the depth axis needs a different channel

A first attempt drew the extent as a **world-space** band tested against the
G-buffer position, inside the `hitType > 0.5` (visible surface) block. That
reached only 1.7 % at depth 12 m — barely over the 1.25 % noise floor — because
a Carve's segment runs *down into solid ground*: the only fragments it can ever
mark are the sliver of surface at the top, and the rest is occluded by the very
geometry the user is aiming at.

The fix is a **screen-space** marker in `shaders/post.comp`: project the hit and
the volume's far end with the same `dot(rel, basis) / dot(rel, forward)`
convention `splat.vert` and `ui/gizmo_math.cpp` use, stroke the line between
them, and put a tick on the far end. Drawn on *every* fragment, deliberately
**not** gated on `hitType`, so intervening geometry cannot hide it — which is
the whole point, since the question is "how deep will this hole be".

Result: depth 0.5→6 m moves **1.74 %** of the frame (Carve) against a measured
noise floor of **0.008 %**. Projecting in the shader rather than on the CPU
also means the indicator cannot drift from the trackball's own projection.

### Two failures the gate caught that my own measurement could not

Both were found by `check_depth_sensitivity`, and both are worth more than the
fixes themselves.

**A wide soft gradient is invisible to a threshold.** The first marker used a
broad `smoothstep` falloff. Under an *exact* byte diff that looked like 8.60 %
of pixels changed; `diff_stats()` read 0.90 % and failed. Both were correct:
`diff_stats()` defaults to `thresh=10`, and a gentle gradient moves most of its
pixels by only 1–10 codes. So: a small **fully-opaque core plus a short halo**
(1.5 px core, 5 px halo). A broad gradient covers a lot of area and still reads
as nothing.

**A deep brush's far end leaves the frustum.** Carve's far end is `depth` metres
*below* the surface. With a camera looking along a shallow downward angle, a
6 m-deep end projects far outside the view — so the line crossed only a few
pixels of frame before leaving it, and every depth drew the same stub
(**0.67 %** between 0.5 and 6.0 m). Clamping both projected ends into the
viewport fixed it (→ 1.75 %).

**Saturation, and why it needed two channels.** A screen-space line is bounded by
the frame, so once the far end clamps to an edge, two large depths can draw the
same picture — 6 m and 12 m were indistinguishable. Fixed on both sides:

- In the viewport, the clamped end keeps *moving along the edge* as depth grows
  (toward a corner), and a short **cut-off cap** is drawn across the axis
  whenever the true far end is off-screen, so a clamped terminus never reads as
  "the hole ends here". 6 m vs 12 m is now **3.0 %**, gated by
  `check_depth_sensitivity("carve_deep", 6.0, 12.0)`.
- The **sidebar footer** spells the depth out numerically (`carve 15vox 1.5m`).
  A number cannot saturate, so it is the channel that stays exact at every depth
  — and it is the readout that is still visible while the pointer is over the
  panel, where the 3D preview runs on the latched hover.

### Measuring the floor with the gate's own instrument

The first `NOISE_FLOOR` constant, `0.02`, was a guess taken from the *unfiltered*
noise. Measured properly with `diff_stats`:

| measurement | thresh=0 | thresh=10 |
|---|---|---|
| identical config ×2 (noise floor) | 2.64 % | **0.008 %** |
| Carve depth 0.5 → 6.0 m (signal) | 5.37 % | **1.74 %** |

The per-channel threshold is what removes the TAA jitter, so comparing a
`thresh=10` signal against an *unfiltered* floor overstates the noise by ~300×
and rejects a signal that is 217× above the real floor. The constant is now
0.005 (60× over the measured floor, ~3× under the signal).

Two habits to keep:

- When hand-measuring something a gate also measures, use **the gate's own diff
  function and threshold**. A stricter ad-hoc comparison is not more rigorous; it
  produces a number that looks like a regression, and the tempting move is to
  "fix" code that was already correct.
- Measure constants rather than estimating them. Both `NOISE_FLOOR` and the
  marker geometry were wrong on the first attempt and neither was obvious.

## The preview must survive sidebar interaction

Hover picking is gated on `!io.WantCaptureMouse` — correct, since a click on a
slider must not stamp the scene. But the *preview* was reading the same gated
`m_hoverHit`, so **reaching for a sidebar slider erased the highlight for
exactly as long as the brush was being resized**. You dragged Width, the tint
vanished, you released, and the new size appeared — so the size you were
setting was never the size you saw.

`App` now keeps `m_latchedHover`: the last pick the pointer made *in the scene*.
The preview falls back to it when the pick is suppressed by UI capture. It is a
**preview-only** feed — `applyEditLive()` still requires a live hit, so a
latched preview can never stamp. Being world-anchored, it stays glued to the
same surface cells while the camera moves.

## The SVO backend had no preview at all

`setBrush()` fed only `m_splatPass`; there was no SVO equivalent, so in
`--mode svo` (or after pressing `F`) every size and depth change was invisible
with no indication why. The `BrushUBO` is now bound at **13** in the SVO
pipeline with a byte-identical `std140` layout, and `inBrushVolume` moved to
`shaders/common_surfel.glsl` so one definition serves both. Binding *numbers*
are deliberately shared across the two pipelines (as 22/23 already were) so a
single declaration works in both.

SVO tests the volume against the **raymarch hit point**, not a per-surfel
centre — a strictly closer match to the CPU cell set than the splat backend's
per-surfel approximation.

Note the SVO descriptor pool was under-specified while this worked: three
uniform bindings (8 selection, 13 brush, 23 atlas table) against a pool sized
for 1, and no `COMBINED_IMAGE_SAMPLER` entry at all. It only succeeded by
driver leniency. Now sized to match the layout.

## Gates

`tests/live_edit_check.py` pins all of it, because the pre-existing
`check_preview` only asserted *a* highlight existed *at one fixed size* — which
is exactly why all three bugs survived:

- `check_depth_sensitivity` — same preview at two depths, demands a change
  above the noise floor.
- `check_svo_preview` — the SVO frame must change, and red-dominantly (proving
  it is the Delete tint, not a broken frame).

All three are reachable on their own via `live_edit_check.py --only preview`,
which is the `test-preview` group — a preview or post-pass change no longer
implies the whole (world-load-dominated) `test-live-edit` matrix. See
[[concepts/focused-test-groups]].

## Instrument notes

- **Noise floor depends entirely on the threshold.** At `diff_stats`' default
  `thresh=10`, two runs of an identical `--shot` differ by **0.008 %**; at
  `thresh=0` (exact bytes) by **2.6 %**. State which one you used. The earlier
  "~1.25 %" figure quoted in a few places is an unfiltered exact-diff number
  from a different cell and camera — do not mix it with `thresh=10` results.
- Debug view 15 passes through tonemap + TAA, so its magenta is **not**
  `(255,0,255)`. A tight threshold reads a real 2900-px mask as *zero* pixels.
  Match loosely: `r-g > 20 and b-g > 20`.
- A **top-down camera foreshortens a vertical depth line to a point**. Judge the
  depth marker from an oblique view (the standard `CAM` in
  `live_edit_check.py`), or it reads as ~0.7 % and looks broken when it is not.
- The indicator **saturates once the far end pins to a viewport edge** — a
  screen-space line cannot be longer than the frame. The cut-off cap plus the
  moving clamped end keep large depths apart (6 vs 12 m = 3.0 %), and the
  footer number is exact regardless. A gate comparing two *very* large depths
  would still be the weakest case; that is why the deep pair is 6 → 12 m rather
  than 20 → 40 m.
