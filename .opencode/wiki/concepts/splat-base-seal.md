---
title: Splat base-seal architecture (why silhouette fading is hard)
tags: [splat, rendering, depth-resolve, seal, silhouette, blending]
sourceRefs: [shaders/splat.frag, shaders/common_surfel.glsl, shaders/splat_tile_render.comp, src/render/splat_pass.cpp]
lastReviewed: 2026-10-01
---

The Gaussian-surfel backend is a **depth-resolved 3-pass composite**, and the
non-obvious part is what the "opaque base" pass (`PASS_MODE 4`) actually
stands for:

1. `PASS_MODE 3` prepass — seeds the nearest **full-disk plane** depth (no
   alpha gate), which feeds the Hi-Z pyramid, the water test, and the resolve.
2. `PASS_MODE 4` base seal — `EQUAL` against that depth, writes `vec4(col, 1)`
   with an **opaque replace, no blend**.
3. `PASS_MODE 1` band — `LESS` with a rasterizer depth bias of
   `-VF_SPLAT_DEPTH_TOL` (~10 cm), blends the front surface's Gaussian
   coverage.

Because the band rejects everything behind `nearest + tol`, **no occluded
geometry is ever drawn**. The alpha-1 seal is therefore not just the surface's
colour, it is the stand-in for *everything behind the front band*. Two
consequences that are easy to get wrong:

- **Making the seal translucent reveals the sky, not the real background.** The
  HDR target starts as the fullscreen sky pass; a partially transparent seal
  blends against *that*, so fading an overhang in front of a hillside produces
  a sky-coloured fringe, not the hillside.
- **There is no "is this fragment supported?" query available.** The prepass
  depth includes the overhang itself, so depth cannot answer it. The stencil
  attachment *is* allocated and cleared (`D24_UNORM_S8_UINT`,
  `pStencilAttachment` bound in both scopes) and was noted in `recreateDepth`
  as "stencil to tell interior rims apart from silhouette rims" — it is the
  natural place for a real coverage count, still unused.

## The measured frontier (2026-10-01)

Two independent mechanisms, measured separately on the reference view against a
**0.18/255 same-binary noise floor**. "Interior" = pixels ≥6 px inside the splat
footprint. "Leak share" = fraction of changed pixels whose change is
blue-dominant (sky showing through) rather than a shading shift.

| mechanism | knob | silhouette bleed | interior | leak share |
|---|---|---|---|---|
| edge window (rolls alpha to 0 at the clip) | `VF_SPLAT_EDGE` (0.6) | **1.00×** | +3.19/255 (69k px) | ~0% (neutral) |
| base declines to seal thin fragments | `VF_SPLAT_SEAL_ALPHA` (0.5) | **1.28×** | +5.60/255 | 57% |
| both | | 1.28× | +7.77/255 | 57% |

Read this carefully: **the edge window alone buys nothing** (1.00×). It fades
the outer band of every disk, but the opaque base seal then covers the whole
disk including that faded band, so the silhouette looks exactly as before. The
seal decline is the only thing that actually uncovers a silhouette — and it is
also the only thing that costs interior fidelity. Shipping the window on by
default would have been paying +3.19/255 for literally zero visual change, so
**both default to 0**.

### Why the seal threshold cannot be tuned away

The base pass only ever sees the **single nearest fragment** at a pixel. It
cannot know that a *different*, higher-alpha fragment covers the same pixel, so
any per-fragment threshold is a guess:

- thresholds 0.65 / 0.8 / 0.9 all produce the **same frame** (interior +6.8…7.3).
  This is why the test must read the **un-windowed** kernel alpha
  (`alphaKernel`): with the window folded in, the window dominates alpha at
  every radius and the "threshold" stops being a threshold.
- `VF_MICRO=0` does not help (interior 5.96 vs 5.60), so the sub-centimetre
  foliage grains are **not** the cause — they were the obvious suspect and they
  are innocent.

### What would actually fix it

A real coverage count. The `D24_UNORM_S8_UINT` stencil is **already allocated,
bound as `pStencilAttachment`, and cleared** in both render scopes, and
`recreateDepth`'s own comment names this exact use: *"stencil to tell interior
// rims apart from silhouette rims"*. Counting overlapping disks there would
let the seal be "declined only where nothing else covers this pixel" — the real
discriminator. That is a structural change to the prepass and is **not**
implemented; the knobs are staged for it.

See also [[concepts/splat-edge-fade-measurement]] for the metrics and the
noise floor, and [[concepts/detail-pipeline]] for the surfel layout.