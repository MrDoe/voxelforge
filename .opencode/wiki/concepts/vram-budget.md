---
title: Renderer VRAM budget (measured)
tags: [vram, memory, renderer, surfels, svo, tile-path, 3dgs]
sourceRefs:
  - src/app/rhi/surface.cpp
  - src/render/splat_pass.cpp
  - src/render/splat_pass.hpp
  - src/app/world/world_textures.cpp
  - /tmp/opencode/fps-default.log
lastReviewed: 2026-11-18
---

# Renderer VRAM budget (measured)

The renderer's steady-state footprint at the shipped hamlet, 960x540, is
**~0.55 GiB** (down from ~0.89 GiB after the VRAM Tuner pass). It is *not*
the cause of dark window captures on this host; the local LLM workers are.
Measured with `nvidia-smi` on 2026-11-18: RTX 4090 Laptop, 16376 MiB
total, 8877 MiB used, 7067 MiB free. Foreign holders: qwen3-vl-8b worker
7688 MiB, embeddings worker 918 MiB, VoiceStudio 238 MiB.

## Where it goes

| Allocation | Size | Gate |
|---|---|---|
| Surfel buffer, 2,854,325 surfels @ 80 B + per-chunk pad + free region | ~261 MiB | always |
| Compaction indirection buffer, 4 B/slot, explicit growth in `growOpaque` | ~13 MiB | always |
| SVO world pools (log: 171.2 MB) | 171 MiB | always |
| Height 1024^2 rg32f (8) + objVol 256^3 r8 (16) + irrVol 64^3 rgba32f (4) | 28 MiB | always |
| Texture atlas 512^2 x 17 layers + mips | 23.8 MiB | always |
| Fur cards 107,055 + micro worlds/mesh | 15.7 MiB | always |
| Full-res targets (offscreen, TAA x3, hdr/gpos/gnorm, galb/gmat, ssaoAo, ssrOut, fxScratch, depth, Hi-Z) | 42 MiB | mostly always |
| OIT targets x4 rgba16f | 7.03 MiB @ 640x360 | lazy: only in `--mode 3dgs` / F-cycle |
| Swapchain x3 | 6 MiB | always |
| Tile counts (2040 tiles x 4096 x 4) + dup stream (12M u32) | 80.6 MiB | gated on `VF_TILE=1` |

Source of the surfel/SVO/fur counts: `/tmp/opencode/fps-default.log` lines 9,
18-22, 59. Target sizes derive from `App::createOffscreen` in
`src/app/rhi/surface.cpp`; tile sizes from `SplatPass::initTileResources` and
`createDepthResources` in `src/render/splat_pass.cpp`. OIT lazy creation logged
as `3dgs: created OIT targets 640x360 (7.03 MiB)`. Compact indirection now
4 B/slot with explicit growth in `growOpaque` (was 80 B/slot, a 20x
over-allocation).

## Verdict

- The app now adds ~0.55 GiB (was ~0.9 GiB): surfel 261 + compact 13 + SVO
  171 + volumes/atlas/fur 68 + non-OIT targets 42 + swapchain 6 MiB. OIT
  targets (7.03 MiB @ 640x360) are created lazily only in `--mode 3dgs` /
  F-cycle; tile resources (80.6 MiB) are gated on `VF_TILE=1`. The compaction
  indirection is no longer a 20x over-allocation — it is 4 B/slot with
  explicit growth in `growOpaque`.
- A dark window with a correct offscreen `--shot` under VRAM pressure is a
  contention/scheduling symptom driven by the ~7.7 GiB VLM worker, not by
  renderer bloat. Check `nvidia-smi` before editing shaders
  ([[concepts/demo-capture]]).
- **Reclaim already landed.** The remaining headroom levers are the atlas
  (23.8 MiB, 512^2) and the fur cards (15.7 MiB); neither is urgent.
  The compaction-buffer quirk on `writeCompactIdentity` is now resolved by
  the resize-on-`growOpaque` change — do not re-introduce the 80 B/slot
  sizing.

## Related

- [[concepts/tile-splat-parity]] — the WIP tile path those 81 MiB serve.
- [[concepts/3dgs-renderer]] — the opt-in OIT mode the targets serve.
- [[concepts/texture-resolution]] — why the atlas is 512^2 (23.8 MiB).
- [[concepts/fur-cards]] — the 12 MiB coat.
