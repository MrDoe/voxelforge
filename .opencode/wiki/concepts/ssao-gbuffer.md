---
title: Screen-space AO (world-scale) + the normal G-buffer
tags: [rendering, ssao, gbuffer, post-processing, splat, svo]
sourceRefs: [shaders/ssao.comp, shaders/ssao_apply.comp, src/render/ssao_pass.cpp, src/app/main.cpp, shaders/splat.frag, shaders/svo_raymarch.comp, tests/ssao_check.py]
lastReviewed: 2026-09-17
---

# Screen-space AO (world-scale) + the normal G-buffer

`H` / render-flag bit 6. **On by default** — `m_renderFlags` is `255`
(bit 5 SSR, bit 6 SSAO, bit 7 detail normals); key `0` resets to 31. The
canonical `visual_check` / `live_edit_check` / `texture_check` shots do not
set `VF_RENDER_FLAGS`, so they now run with SSAO **on**. `ssao_check` drives
it explicitly with `FLAGS_ON = 95`, which means **bit 7 is off in that test**
— a detail-normal regression is not an SSAO regression. Split into two
compute dispatches in `SSAOPass::record`:

1. **`ssao.comp`** — raw AO into the `m_ssaoAo` scratch image.
2. **`ssao_apply.comp`** — 5x5 cross-bilateral denoise (view-depth + normal
   weights) then `base * (1 - strength*ao)` on the post-tonemap LDR image.

## Why it was rewritten (2026-09-17)

The old kernel was a fixed 3–19 px golden spiral with a 2.5 m hard cull,
`1/(1+2L²)` attenuation, a `-0.12` normal bias, and it dropped sky taps from
the denominator. Its world footprint was resolution/view-distance dependent
(millimetres at close range → no contact AO), it haloed at silhouettes, and
the only surface chaotic enough at every pixel scale to read was canopy
foliage — hence "SSAO only looks great for leaves".

## Kernel

- Two **world-space bands**: near crease band = 0.35 x far, far band =
  `VF_SSAO_RADIUS` (default 0.8 m). Per band: 6 directions x 3 steps; the
  radius is projected to pixels via `ppm = 0.5*extentY / (tanHalfFov*z)`,
  clamped to 3..48 px. Per-pixel rotation comes from a deterministic hash.
- Tap test: `h = dot(n, dv/L)`, slope-scaled tangent-plane bias
  (`h*L < max(1.5*VOXEL, 0.02*L)` rejects), smooth range weight
  `exp(-(L/r)²)`, horizon term `s = clamp((h - 0.03)/0.97, 0, 1)`.
- Out-of-bounds, sky and water taps still add their **nominal weight** (they
  dilute, never occlude) — this is the halo fix. Sky and water pixels write
  AO 0 and pass through untouched (water is skipped by design).
- `kAoGain = 5.0` maps the sparse-fan occluded fraction onto [0,1]; a
  distance fade (45→80 m) kills far-field aliasing. Defaults:
  `VF_SSAO_STRENGTH=0.6`, `VF_SSAO_RADIUS=0.8`, `VF_SSAO_BLUR=1`,
  `VF_SSAO_DEBUG` (1 = raw pre-filter AO, sky/water read 0; 2 = G-buffer
  normal).

## Normal G-buffer (`m_gnorm`, rgba16f)

Written by **both** backends next to `m_gpos` and consumed by SSAO and SSR
(SSR falls back to finite differences on a degenerate normal). Facts:

- `splat.frag`: 3rd colour attachment (`oGNorm`); solid = post-flip surfel
  normal (pre-flora), water = flat `(0,1,0)` so SSR's water reflection stays
  plane-stable, sky = 0. `svo_raymarch.comp`: binding 12, `calcNormal(p)` /
  `(0,1,0)`. Tile path (`VF_TILE=1`): binding 21.
- The splat pipelines have **3 colour attachments**: `createPipelines`
  formats/blend states + the three `VkRenderingAttachmentInfo` blocks in
  `record`/`recordTile` + `colorAttachmentCount` must always agree.
- `m_gnorm`/`m_ssaoAo` are transitioned UNDEFINED→GENERAL per frame at the
  same sites as `m_hdr`/`m_gpos` (contents discarded).

## Backend parity note

At identical settings the SVO reference shows ~3–4x the AO magnitude of the
splat path (e.g. the house shot: ~7.5% vs ~2% mean luminance drop), because
SVO uses exact per-pixel SDF-gradient normals on stepped geometry while
splats use smoothed surfel normals plus micro disks. This is expected — the
voxel steps genuinely self-occlude. Don't "fix" it with per-backend strength
hacks.

## Verification

`tests/ssao_check.py` (ctest target `ssao_check`): hero/house/water x
(splat, svo) x (off / on / debug) at 480x270; asserts sky-classified pixels
stay unchanged (mean ≤ 1 code, max ≤ 20), the land shots drop 0.1–9% mean
luminance, black-in-silhouette stays < 5%, and the debug AO fires on solid
pixels but reads ~0 on sky. Cost: ~1.2 ms at 720p / ~2.5 ms at 1080p (two
dispatches, `fx` bucket in the GPU profiler).

Related: [[entities/svo-render]], [[concepts/shading-model]],
[[concepts/detail-pipeline]].
