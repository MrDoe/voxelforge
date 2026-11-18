---
title: 3DGS renderer (edge-only OIT composite)
tags: [rendering, 3dgs, oit, backend]
sourceRefs:
  - shaders/splat.frag
  - shaders/oit_composite.comp
  - src/render/oit_composite_pass.hpp
  - src/render/oit_composite_pass.cpp
  - src/render/splat_pass.hpp
  - src/render/splat_pass.cpp
  - src/app/app.hpp
  - src/app/cli/args.hpp
  - src/app/frame/record_interactive.cpp
  - src/app/frame/record_headless.cpp
  - tests/dgs_check.py
lastReviewed: 2026-11-17
---

# 3DGS renderer (`--mode 3dgs`)

A third render backend alongside `splat` (depth-resolved opaque Gaussian
disks) and `svo` (chunked-SVO reference). Opt-in via `--mode 3dgs` (alias
`dgs`) or by cycling `F` (Splats → SVO → 3DGS). It renders the **same surfel
set** as `splat` but adds a weighted-blended order-independent transmittance
composite at silhouette edges only. Solid interiors are byte-identical to
`splat` — the user requirement was explicitly *"only the edges of silhouettes
shall be transparent"*.

## Why it exists

`splat` is an opaque-surface approximation: a depth prepass writes the nearest
full-disk plane depth, an opaque base pass (depth EQUAL) writes the nearest
fragment, and a Gaussian band (depth LESS + bias) blends only the front
surface. Anything deeper than the resolve band is depth-culled, so a
translucent rim has no accumulated second layer to blend with and the sky
shows through. A naive global OIT composite fixes that but repaints the whole
frame: every splat along a ray contributes to the averaged colour, so the
front and back of an object blend and interiors wash out (measured 30.98/255
whole-frame diff, 76% of pixels moved). The shipped design keeps the good
opaque frame and substitutes OIT only where coverage is sparse.

## Composite math

Four RGBA16F targets plus a base copy:

- `m_oitAccum` — additive premultiplied colour + alpha, `Σ (α·w·c, α·w)`
  where `w = exp(−t·k)·64` is a depth weight (`k = VF_OIT_WEIGHT`, default
  0.02) that keeps near splats dominant in the average.
- `m_oitReveal` — starts at 1, each disk multiplies it by `(1−α)` via a
  `ZERO / ONE_MINUS_SRC_COLOR` blend, so it ends as `T = Π(1−α)`.
- `m_oitBackdrop` — the sky, drawn by a dedicated one-attachment sky pipe.
- `m_oitBase` — a copy of the standard `splat` frame (`vkCmdCopyImage` from
  `m_hdr`), so interiors can be preserved.

Composite (`shaders/oit_composite.comp`):

```
avg  = accum.rgb / max(accum.a, 1e-5)
oit  = avg * (1 - T) + backdrop * T
edge = smoothstep(0.08, 0.6, T)          // sparse coverage only
out  = mix(base, oit, edge)
```

`T` is used for the edge mask, **not** `accum.a`: the latter carries the depth
weight (mean ~549 on a solid surface) and is not a raw coverage count. A solid
interior has `T≈0` → `edge=0` → base (identical). A lone low-alpha silhouette
splat has `T` near 1 → `edge=1` → OIT. Sky also has `T=1`, but there
`oit == base == backdrop`, so the mix is harmless.

## Implementation

### Shader pass (`shaders/splat.frag`, PASS_MODE 5)

Runs with **no depth test at all** — every overlapping splat is shaded, which
is the point. Writes premultiplied depth-weighted colour+alpha into attachment
0 and alpha into attachment 1 (the reveal target). oGPos is written at the
shared site so the reveal value is not clobbered by the G-buffer position
write. The same `shadeSurfel()` lighting, guidance lanes and backlit
transmission are used as the shipped path.

### Composite pass (`shaders/oit_composite.comp`, `src/render/oit_composite_pass.{hpp,cpp}`)

One compute dispatch over the screen rect, 5 storage-image bindings, **no push
constants** (so it sidesteps the `RaymarchPush` 128-byte offset trap). Bindings
must all be present in the descriptor set layout — see the `bindingCount` gotcha
below.

### Splat pass (`src/render/splat_pass.{hpp,cpp}`)

- New pipelines `m_oitSkyPipe` (1 attachment) and `m_oitPipe` (2 attachments:
  additive + reveal-multiply), both with no depth.
- `recordOit()` runs AFTER a normal `record()` this frame (which filled the
  G-buffer + depth and computed the culled chunk draws), draws sky to the
  backdrop and all surfels to accum/reveal from the CPU draw list (ignoring the
  GPU cull's compaction — OIT shades everything anyway).
- `setOitTargets()` takes the four App-owned image views.

### App / RHI (`src/app/app.hpp`, `src/app/rhi/surface.cpp`)

Four new RGBA16F targets `m_oitAccum` / `m_oitReveal` / `m_oitBackdrop` /
`m_oitBase`, created in `createOffscreen()` and rebound on resize; OIT pass
init/destroy/descriptor wiring. `record_interactive.cpp` and
`record_headless.cpp` both add a Dgs branch that transitions targets, calls
`recordOit()`, copies `m_hdr`→`m_oitBase`, and runs the composite before the
post/FX chain.

## Measured verdict (2026-11-17)

Gate: `tests/dgs_check.py` (ctest `dgs_check`, label visual), with
`VF_BASELINE_BIN` pointing at a pristine HEAD binary:

| view | interior diff | whole-frame >10 px |
|---|---|---|
| hero | 0.046/255 | 0.05% |
| house | 0.000/255 | 0.28% |
| water | 0.001/255 | 0.02% |

Default `splat`/`svo` paths are byte-identical to HEAD. `VF_OIT_DUMP=<prefix>`
logs the accum/reveal/base statistics for debugging.

## Photorealism ceiling (measured, not argued)

Combining 3DGS with the photoreal flags
(`VF_RENDER_FLAGS=127 VF_LINEAR_FX=1 VF_AUTOEXPOSE=1 VF_TRANSMISSION=1`) moves
40% of pixels (mean 11.7/255) by improving tone/exposure, and adds only the
same 0.29% edge pixels. But a vision judge still reads the result as
**stylized blocky voxel geometry**, not photographic. Kernel sweeps are weak
(σ 0.5→1.4 moves 6.7% of pixels; extent 1.02→1.5 moves 39% but reads only
"soft/blurry"). The ceiling is geometry + stylized textures, not the composite
or post chain — closing it needs different assets or a learned renderer, not
more parameter tuning.

## Gotchas found while landing this

- `VkImageCopy ic{}` has `layerCount=0` → silent no-op without validation
  layers; set both subresources to `{ASPECT_COLOR, 0, 0, 1}`.
- Adding a 5th descriptor binding but leaving `bindingCount=4` made one image
  read zero (black interiors); update array, count, pool size, write loop and
  shader together.
- `attachColour()` hardcodes `VK_IMAGE_LAYOUT_GENERAL`; the OIT targets are
  transitioned to `COLOR_ATTACHMENT_OPTIMAL`, so the attachment info layout
  must be overridden to match.
