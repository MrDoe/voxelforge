---
title: Motion blur and the unseeded previous-frame camera
tags: [render, motion-blur, prev-cam, bug-fix, measurement]
sourceRefs: [src/render/motion_blur_pass.cpp, src/render/taa_pass.hpp, src/app/frame/frame.hpp, src/app/frame/record_headless.cpp, src/app/frame/record_interactive.cpp, src/app/frame/run.cpp, src/app/frame/run_startup.cpp, shaders/motion_blur.comp]
lastReviewed: 2026-10-09
---

`shatters/motion_blur.comp` is a straightforward reproject-and-gather shader. The
defect was never in it: the **camera basis it reprojects against was
uninitialised** in the headless path.

## Symptom

`VF_MOTIONBLUR=1` on `--shot`, a completely static camera, smeared the whole
frame. 960×540 A/B, on-vs-off, mean L2/code:

| view | before | after |
|---|---|---|
| hero | 6.76 | 0.045 |
| house | 16.54 | 0.029 |
| water | 15.92 | 3.50 |

The residual water value is the analytic floor for that view (see below), not a
remaining defect.

## Root cause

`App::m_prevCam` is the basis of the camera that rendered the *previous* frame,
read by two consumers: TAA's history reprojection and the motion-blur velocity
field. It was written **only inside the TAA branch** of
`App::recordInteractiveFrame`, so:

- **interactive with TAA off** (now the default, see
  [[concepts/render-defaults-off]]): `m_prevCam` was never written at all.
- **headless**: never written either, because `recordHeadlessFrame` is a
  separate function that never touched it.

The untouched value is `PrevCam`'s default: `pos = (0,0,0)`,
`right = +X`, `up = +Y`, `fwd = +Z`, `tanHalfFov = 1.0`. That is a camera
sitting at the world origin looking down +Z. Reprojecting a world position
through it against the real camera at ~(1, 2, 1.5) looking at (5.3, 1, 11.3)
produces `cz = dot(rel, +Z)` on the order of metres, a `prevPx` tens or hundreds
of pixels away, and therefore a large velocity at nearly every pixel — exactly a
full-frame smear on a still scene.

Every previous trace of this being "TAA-only" is why it survived: the pass has
always been interactive+TAA-gated, and no headless test ever enabled it
(`VF_MOTIONBLUR` appears in no test script, and `grep VF_MOTIONBLUR` matches
only docs and the one `run_startup.cpp` reader).

## Fix

`vf::PrevCam` (was `TaaPrevCam`, in `render/taa_pass.hpp`; the render layer owns
it because the app's `frame.hpp` cannot be included from `src/render/*`) plus one
`App::advancePrevCam()`:

- **seeded** in `App::run()` right after `setupCameraAndShots()`, so the very
  first frame reprojects against a valid basis;
- **advanced at the end of every recorded frame on both paths** — the
  interactive update moved out of the TAA branch, and the headless path got the
  same call.

The aspect ratio comes from `m_swapchain.extent()`, matching what both frame
recorders put in `push.a` — that is the projection the G-buffer world positions
were rendered with, so a mismatch would silently mis-scale horizontal velocity.

`shaders/motion_blur.comp` was **not** touched. Its math is right for a static
camera: for a point on the centre ray, `proj(gp)` returns exactly the pixel
centre by construction (verified analytically: camera 8.5,0.6,8.2 → 4.5,-1.1,6.8
projects a 10 m centre point to (480.0, 270.0)). The gather loop, the velocity
clamp and the `len < 0.75` no-op threshold are all fine.

## Verification

- The matrix above: hero/house collapse into the same-binary noise floor
  (0.0088).
- Cross-correlation of the changed water pixels finds no dominant direction
  (best scores 16/15/11 px over an even spread), so it is not a coherent smear
  with a discoverable velocity.
- The water view's residual is float16 quantisation of the G-buffer, not a bug:
  `m_gpos` is `R16G16B16A16_SFLOAT`, and a water point ~4 m out quantises by up
  to 0.0039 m. Reprojecting that through a 60° FOV gives ≈0.12 px of apparent
  velocity — below the shader's own 0.75 px threshold, so it cannot cause the
  blur; the residual there comes from the *water pixels'* own quantisation
  (they are ~1.5 m below the camera, so the same absolute error subtends ~3×
  more angle). If water still reads as smeared, that is the ceiling and it needs
  an RGBA32F G-buffer, not a shader change.

Note the SVO backend shows the same order of magnitude (mean 2.99), which is the
expected conclusion given the defect was entirely CPU-side: the shader and both
backends were innocent.

Related: [[concepts/render-defaults-off]] (the TAA default flip that exposed
this), [[concepts/taa-history]] for the NaN-sticky history variant of the same
"single consumer" trap, and [[concepts/measurement-provenance]] — the number
quote without this page's "before" column would have looked like a 6-16 L2
smear that "still works".
