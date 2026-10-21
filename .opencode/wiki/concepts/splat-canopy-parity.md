---
title: Splat canopy parity - why the splat backend paints the tree black
tags: [splat, svo, shading, artifact, canopy, regression-guard]
sourceRefs: [shaders/splat.frag, shaders/common_splat.glsl, shaders/common_surfel.glsl, src/voxel/surfelize.cpp, tests/visual_check.py]
lastReviewed: 2026-10-21
---

# The "slots in the tree" artifact (2026-10-21)

The user reported *slots/slits* in the tree of the lakeside hamlet. The
splat backend showed them; the SVO reference did not ("there are no slits on
SVO reference"). They read as dark slivers in the canopy, which is what made
them look like slots in the ground under the tree.

## The locked repro

The camera was reconstructed from the HUD line in the user's own screenshot
(`Pos -4.2, 0.9, 14.6` / `Head 187 deg S`) plus the horizon height in the
frame. It reproduces the user's frame at **Pearson r = +0.74** against
`.opencode/tmp/invest/user_splat.png`:

```
--cam -4.2 0.9 14.6 -5.404 -0.488 4.792 --width 1280 --height 800
```

Two traps when comparing frames:

- The app's canvas is the **full window**; the sidebar is an *overlay* on its
  left ~334-346 px. A naive 16:9 comparison of the whole PNG (which is
  1280x810: a 10 px WM title bar + the canvas) misaligns by a fifth of the
  frame and reads as "uncorrelated" (r ≈ 0.03) for a perfectly good camera.
- The user's PNGs live in `.opencode/tmp/invest/` (1280x810). The
  `1600x932` PNGs in `.opencode/tmp/` are a *different* view.

## The instrument: the near-black mask

The metric is **splat-only near-black**: pixels below luma 30 in the splat
frame that are at or above luma 30 in the SVO frame. One-directional - the
SVO-only set is 0.20% of the frame, so the splat backend is painting black
where the reference paints mid-tones.

| frame (1280x800, with the user's overlay) | mask | mean luma at the mask | SVO at the same px |
|---|---|---|---|
| splat | 76,268 px (7.45%) | 19.2 | 62.9 |
| splat, render bit 8 off | | 25.6 (68% still <30) | |
| splat, bits 8+1 off | | 36.0 (50.8% still <30) | |
| SVO | - | - | 62.9 (0% <30) |

At the test's own resolution (640x400, `VF_NO_OVERLAY=1`): **21,785 px
(8.51%)**, mean luma 19.9 vs the reference's 60.7.

The masked pixels are **warm dark brown** ((26,14,2), (31,17,2), (36,20,3))
where the SVO shows neutral grey-brown ((70,61,60), (54,44,39)) - the same
material family, a very different brightness. Not a coverage hole: a hole
would show sky or the background, and the SVO shows *lit geometry* there.

## What causes it (ranked by measurement)

1. **Disk reach - the dominant term (31 of the 44 luma gap).** A surfel
   from a *deeper*, shadowed layer wins the depth prepass over the true
   front surface, and its 1.4-voxel disk darkens the lit cell next to it.
   `VF_SPLAT_RADIUS=0.5` (0.7 voxel) swings the mask from 18.65 to 49.66
   luma - nearly double the whole bit8+bit1 span. `VF_SPLAT_OPACITY=0.5`
   (19.59) and `VF_SPLAT_SIGMA=0.25` (19.06) are inert, so the error is
   *spatial reach*, not blending or kernel width. A global shrink is NOT the
   fix: the silhouette IoU against the SVO is 0.4800 at the default radius,
   0.4812 at 0.7, 0.4837 at 0.85, but 0.4497 at 0.5 - it buys luma with 84k
   over-covered pixels traded for 74k holes. The landed direction is a
   *shading-variance radius*: shrink only where the baked shadow varies
   across the face neighbours.
2. **The enclosed-space test (bit 8), worth ~6 luma at the mask.** The
   splat backend decides "is this surface enclosed?" by marching the
   **coarse 0.4 m object volume** (`skyVisibilitySplat` in
   `common_splat.glsl`), so a porous tree canopy always reads as a sealed
   room: its sky ambient is scaled to 0.16 and its IBL to 0.04. The SVO
   backend marches the *exact* geometry and says "open sky". A tree canopy
   is not a sealed room - that is a real bug, independent of the artifact.
3. **The baked sun shadow is binary, the SVO's is a 16-tap PCF.** The splat
   bakes one verdict per surfel (`shadowMarch` over the exact oracle, 0 or
   1); the SVO computes `softShadow` per *fragment*, so light reaches the
   canopy through the gaps between leaves. The splat path shades a
   back-facing leaf with no sun at all.

## The dead ends (each killed by a measurement, not by taste)

- **Coverage holes at the chunk boundary.** The user's six picks were flat
  sand tops straddling the `z=639/640` chunk boundary, each with exactly one
  supported splat (`r=0.14`, `n=(0,1,0)`); the store surface is flat across
  the boundary; `VF_SPLAT_DEBUG=1` (flat coverage) shows **zero** uncovered
  pixels at grazing views over that spot.
- **A stale height texture.** `VF_SPLAT_DEBUG=11` (heightfield residual) is
  uniform over the seam - redness 0.00 on every row of an 800x800 top-down
  frame centred on it.
- **The micro overlay.** Ruled out geometrically: the three micro layers sit
  at x = +11.55..+12.3, ~80 degrees off-axis at the locked camera, and the
  overlay only overwrites a pixel where a micro surface is *nearer*.
  (There is no `VF_MICRO` env switch in this tree - setting it renders the
  default twice, which is how it looked "ruled out" at first.)
- **Nondeterminism.** Two identical `--shot` runs differ by at most 5/255 on
  ~1.3% of the pixels, scattered - no line-shaped difference.

## The guard

`tests/visual_check.py` now renders both backends at the locked camera and
asserts the near-black mask stays at or below its documented baseline
(21,785 px, +15% margin for the per-run noise). It reports the count and the
mean luma at the mask. Tighten the baseline when the artifact is fixed;
never loosen it silently.

## See also

- [[concepts/enclosed-space-lighting]] - the bit 8 enclosure contract
- [[concepts/baked-sun-shadow-contract]] - the binary per-surfel shadow
- [[concepts/splat-edge-fade-measurement]] - how to measure a splat change
- [[concepts/headless-render-mouse-safety]] - render without stealing the mouse
