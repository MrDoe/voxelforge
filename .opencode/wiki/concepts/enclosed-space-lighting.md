---
title: Enclosed-space shading and light sources
tags: [shading, lighting, cave, ambient, ibl, worldfile, manifest, vulkan]
sourceRefs: [shaders/common_base.glsl, shaders/common_svo.glsl, shaders/common_splat.glsl, src/voxel/worldfile.cpp, src/voxel/worldfile.hpp, src/render/splat_pass.cpp, src/render/svo_pass.cpp, src/app/rhi/surface.cpp, src/app/world/world_layers.cpp, tests/test_worldfile.cpp, docs/rendering.md]
lastReviewed: 2026-10-06
---

# Why a cave stayed as bright as a meadow

The renderer had exactly one light: the sun. Everything else — the analytic
`skyIrradiance()` taps and the pre-filtered environment cubemap
(`iblContribution()`) — is **ambient with no occlusion term at all**. Baked AO
only reaches 0.18 m and 0.60 m (`aoBake` / `splatAO`, 2 rings × 4 taps), so it
grounds a contact shadow and nothing more. A cave 3 m across is far outside
that radius: the interior read as bright as open ground, because it *was*
being lit like open ground.

## The fix is two independent things

1. **A one-ray enclosure test** (render-flag **bit 8**, default ON;
   `m_renderFlags` 255 → 511, and `VF_RENDER_FLAGS=255` restores the old
   image bit-exactly).
   `skyVisibilitySvo` / `skyVisibilitySPlat` cast a single ray along
   `mix(bent, +Y, 0.65)` and return 0/1. `enclosed = 1 - vis` then scales
   ambient `mix(1, 0.16)` and IBL `mix(1, 0.04)`.
2. **A light-source lane**, because darkening alone only makes caves dim,
   not lit.

### The cave fill is load-bearing, not cosmetic

Removing the daylight without adding anything back pushed the `house` shot to
**7.21 %** black-in-silhouette and tripped `visual_check`'s 5 % hollow-voxel
gate. The gate is a good proxy for "did you just make everything black", so
the fix is an albedo-scaled fill standing in for wall multi-bounce:

```glsl
col += alb * vec3(0.075, 0.080, 0.090) * enclosed * (0.35 + 0.65 * ao);
```

Measured `house` black-in-silhouette: **2.70 %** (bit 8 off) → **7.21 %** (dark,
no fill) → **4.26 %** (dark + fill).

### The splat sky test must NOT consult the heightfield

`heightAt()` returns the terrain *top*, so `sp.y - heightAt(sp.xz) < 0` is true
for **every** point below the surface — including the floor of any room, and
especially of a room dug into a hillside. Including it in the sky test marks
interiors as underground and diverged from the SVO reference by tens of
points of enclosed coverage. A heightfield cannot have overhangs, so an upward
ray can never be occluded by terrain: the splat sky test uses `objDist` alone.

The **per-light** shadow test is the opposite case and does march terrain — a
lamp behind a hill must not light the far side.

## Light sources

```json
"lights": [
  { "pos": [6.0, 1.6, 11.5], "color": [1.0, 0.55, 0.18],
    "radius": 7.0, "intensity": 6.0 }
]
```

- `worldfile::loadLightManifest` → `std::vector<LightSource>`; max
  `kMaxLights` (16); entries with `radius <= 0` or `intensity <= 0` are
  dropped, not uploaded as lights that can never contribute.
- Packed into `LightUBO` — **528 B**, two `vec4[16]` then `count` at byte 512
  and 3 pad ints, i.e. `alignas(16)` std140. A unit test pins every offset,
  because moving either side of the contract silently reads a garbage count
  rather than failing to compile.
- Binding **25** in all three descriptor sets (SVO, splat forward, splat tile).
  The tile set's binding array is indexed by *binding number*, so a new
  binding at the end means growing `tb[]` **and** `tli.bindingCount` **and**
  the descriptor pool's uniform count — the array is sized to the highest
  index + 1, not to how many entries are filled.
- `applyLights()` lives in `common_base.glsl` and dispatches the shadow march
  through `#ifdef SPLAT_BACKEND`, so forward, GPU-cull and tile all agree.

## The descriptor-ordering trap that cost the feature its first test

`setLights()` writes a descriptor of an already-allocated set. `initVulkan()`
originally called it **before** `m_splatPass.init()`, so on the splat path
`m_ctx` was null and the call returned silently: the manifest logged
"lighting: 1 explicit point lights", the SVO backend lit up, and the splat
backend did not change by a single code. The lamp looked broken, not
uninitialised.

**Rule: anything that writes a descriptor lives after every pass `init()`.**
`App::uploadLightSources()` is now the single writer, called from
`initVulkan()` after the last pass and from `applyWorldReload()`.

## Verification

One lamp inside the cabin, 320×180, camera inside the shell:

| shot | mean luma | pixels < 30 |
|---|---|---|
| splat, no light | 42.01 | 44.86 % |
| splat, 1 lamp | **87.10** | **11.04 %** |
| SVO, 1 lamp | 101.37 | 2.06 % |
| splat↔SVO delta, unlit | −16.68 | — |
| splat↔SVO delta, lit | −14.28 | — |

The light does **not** add backend divergence: the delta is unchanged within
noise, so the two backends agree on what the lamp does.

Cross-links: [[concepts/shading-model]], [[concepts/measurement-discipline]],
[[concepts/ssao-gbuffer]], [[entities/svo-render]].
