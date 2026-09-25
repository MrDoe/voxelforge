---
title: Texture detail normals (render flag bit 7)
tags: [rendering, normals, textures, shading, splat, svo]
sourceRefs: [shaders/common_base.glsl, shaders/common_splat.glsl, shaders/common_svo.glsl, src/app/main.cpp]
lastReviewed: 2026-09-19
---

# Texture detail normals

`detailNormal()` (`shaders/common_base.glsl`) derives a tangent-space bump
from the material texture's **albedo luminance** and perturbs the shading
normal. This is what makes a photo texture read as a *surface* rather than
painted colour — the eye reads relief from shading, not from albedo. Before
it, surfaces beyond the micro-surfel cull were flat-lit
([[concepts/detail-pipeline]]).

**Toggle**: render-flag **bit 7** (value 128), key **B**.
`m_renderFlags` defaults to **255** (was 127). `VF_RENDER_FLAGS=127` disables.

## How it works

A 4-tap Sobel of the sampled luminance over the **same dominant-axis
triplanar projection** `texTriplanar` uses (so the perturbation cannot
shimmer per pixel), stepped by `h = scale * 0.004` (≈2 texels at 512²). The
luminance is treated as a height field: `slope = dL/dx * kDetailRelief`, then
`n' = normalize(n - (uA*g.x + vA*g.y))`, clamped so the tilt can never invert
the normal.

`kDetailRelief` is in metres of relief per unit luminance. It was calibrated
by A/B against the hero shot's Laplacian HF energy:

| kDetailRelief | HF energy | mean \|diff\| |
|---|---|---|
| 0.015 | +3 % | 0.46 |
| 0.06 | +29 % | 1.46 |
| **0.08 (default)** | **~+40 %** | ~1.9 |
| 0.12 | +66 % | 2.57 |
| (an earlier dimensionless gain of 0.5) | +188 % | 6.48 |

The first implementation multiplied the per-metre gradient by a dimensionless
0.5, which pushed almost every slope into the clamp (~51° tilt everywhere) —
the "relief" read as heavy embossing. Scaling by a physical relief height
instead keeps most surfaces in a natural 5–25° range.

## Invariants — do not relax

- **Shading normal only.** `oGNorm` keeps the geometric normal, so SSAO is
  untouched and SSR's water plane-stability holds ([[concepts/ssao-gbuffer]]).
  The water path does not call it at all.
- **Skipped for foliage (mat 8)** — its random facet normals *are* the canopy
  volume.
- **No-op for untextured materials** (`slot < 0`), which is what keeps
  `VF_TEXTURES=0` bit-exact — `texture_check` asserts that.
- Applied **after** `detailAlbedo` at all four call sites (both backends, plus
  the two submerged-floor functions), so the albedo sample still uses the
  geometric normal.

Note: `tests/ssao_check.py` runs with `FLAGS_ON = 95` (bit 7 **off**), so it
never exercises detail normals — a regression there is not a detail-normal
regression.

Cross-refs: [[concepts/detail-pipeline]], [[concepts/texture-atlas]],
[[concepts/texture-resolution]].
