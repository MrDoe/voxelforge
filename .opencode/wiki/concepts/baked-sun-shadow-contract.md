---
title: The baked sun-shadow contract (what the splat path reads instead of marching)
tags: [shadows, sun, splat, surfelize, bake, contract, bit-1, enclosure]
sourceRefs:
  - src/voxel/surfelize.cpp
  - shaders/common_splat.glsl
  - shaders/common_base.glsl
  - shaders/common_svo.glsl
  - src/app/ui/panel_render.cpp
  - src/voxel/voxel_field.cpp
lastReviewed: 2026-10-08
---

> **Provenance (2026-10-08):** synthesised from an **uncommitted working
> tree**. `shaders/common_base.glsl`, `shaders/common_splat.glsl`,
> `src/app/ui/panel_render.cpp`, `src/voxel/surfelize.cpp` and
> `src/render/texture_atlas.{hpp,cpp}` were all modified and uncommitted when
> this was written, so line numbers are as-of that tree. Re-verify before citing
> as committed fact.
>
> **Decision context:** a sun **shadow-map pass** was chosen (by the user, via
> George) as the route to dynamic shadows. That decision is recorded in
> `log.md` under `[2026-10-08] decision` and is explicitly **not** a validated
> result — the two pages arguing the re-bake is cheaper *and* better today are
> not overturned by it, they are unanswered by it. This page is what a shadow
> map has to reproduce.

# The baked sun-shadow contract

The splat backend does **zero per-fragment sun-shadow work** on opaque
surfaces. Every opaque fragment reads a value baked on the CPU at
`rebuildSurfels()` time. That is not an optimisation detail — it is a contract
with four clauses, and any change to shading, live editing, or the render-flag
bit that gates shadows has to respect all four.

Written 2026-10-08 at George's request (rename of "Sun shadows" → "Shadows",
emissive-driven point lights, dynamic shadows) because the rule existed only as
scattered comments. Vega's [[concepts/dynamic-sun-shadows]] covers the cost side.

## Where the value is produced

`src/voxel/surfelize.cpp:419`

```cpp
template <typename FieldT>
float shadowMarch(const FieldT& f, glm::vec3 ro, glm::vec3 rd)
```

Templated on the field so **one rule serves both** paths:

| call site | field | path |
|---|---|---|
| `surfelize.cpp:929` | `VoxelField` | full bake, `buildSurfels` pass 2 |
| `surfelize.cpp:2044` | `ChunkStore` | live-edit store path, `shadeCandidates` |

That sharing is not incidental: the live-edit path re-derives only the edit AABB
± margin, so a divergent rule would re-aim untouched splats. See
[[concepts/live-edit-surfel-parity]] — the same argument that forced the normal
pipeline to be reused verbatim applies to the march.

**Two phases.** Cell-exact Amanatides DDA to 3 m (≤64 taps, visits every cell
like the SVO reference, so thin eaves and logs are never tunnelled; occluded
when `f.sampleWorld(sp).d < -0.02f`), then sphere-tracing to 60 m (≤80 taps,
step `clamp(|d| * 0.7f, 0.04f, 1.0f)`). Verdict is binary: 0 occluded, 1 clear.

## The four clauses

### 1. Backfaces never march — `sh = 1` there is absence of information

```cpp
float shadow = 1.0f;
if (glm::dot(n, sunDir) > 0.02f)
    shadow = shadowMarch(field, pos + n * 0.3f, sunDir);
```

A surfel facing away from the sun keeps `sh = 1` and never marches. The fragment
stage gates on the same predicate. This is the single easiest thing to
misread: `sh = 1` on a backfacing wall does **not** mean "lit", it means
"unmeasured". Any code that consumes `sh` as a lighting fact on an unmeasured
surface is reading noise as signal — which is exactly what happened to the
enclosure proxy before it was given the *raw* baked value
([[concepts/heightfield-blindness-enclosure]]: a carved interior measured
45.26 mean luma with enclosure ON vs 45.84 OFF, because `sh` was 1 precisely
where the answer was needed).

### 2. The origin rides 0.3 m off the surface

`pos + n * 0.3f` here; the SVO twin uses `p + n * 0.35`. A tight offset buries
canopy and thin-object origins inside neighbouring solid and self-shadows. Any
replacement march must keep the offset or it will invent contact shadows under
foliage that the bake deliberately does not have.

### 3. The value that reaches the GPU is not the marched value

After pass 2, a third stage averages the baked verdict and the baked AO over
**face-neighbouring surfels** ("shadow penumbra + AO smoothing"), because the
march is binary per surfel — it draws shadow boundaries along disk shapes — and
the AO steps at staircase-tap granularity, mottling flat walls. Averaging makes
adjacent same-colour splats shade identically.

So a per-fragment GPU march that is *sharper* than the bake will not merely
differ, it will differ **against its own neighbours**. Parity means matching a
spatially filtered value, not a per-fragment one.

Storage: `sl.bent_sh = glm::vec4(bent, shadow)` and `sl.mat_ao = glm::vec4(mat,
refl, rough, ao)`. AO is baked by `aoBake` (2 rings × 4 taps, radii 0.18 / 0.60
m) which mirrors the GPU `splatAO` ring layout on purpose.

### 4. Children inherit the parent's verdict

Edge bridges and material micros take the base cell's baked shadow and AO
([[concepts/detail-pipeline]]), and the chunk layout contract
(`base | edge bridges | material micros`) assumes it. A dynamic-sun path has to
propagate to children or the same surface shades two different values across its
own children.

## What consumes it

- `shadeSurfel` (`common_splat.glsl`) takes `sh` and `shRaw` as parameters and
  marches nothing for the sun.
- `applyFlora` (`common_base.glsl`) re-evaluates the sun only when
  `!useBaked`, reusing `shBaked` otherwise — the flora re-check after
  `grassDetail` perturbs the normal inherits the baked value.
- `aoShEnclosure` (`common_base.glsl`) is the **bit-1 consumer** (see below).

## Bit 1 ("Shadows") now gates point-light occlusion too

`applyLights()` runs `lightVisibilitySPlat` / `lightVisibilitySvo` only when
`(gRenderFlags & 2) != 0`. Before that, point lights marched
unconditionally, so a toggle labelled "Shadows" would have been untrue with the
bit clear. Consequences to write down:

1. **Bit 1 is now an input to enclosure.** `aoShEnclosure` requires
   `(gRenderFlags & (256 | 3)) == (256 | 3)` and folds the RAW baked `sh`:
   `clamp((1 - ao) * (1 - sh), 0, 1)`. Toggling bit 1 therefore changes how
   caves and interiors shade, not just cast shadows. A test that flips
   "Shadows" and sees interiors move is reading this proxy, not a rename bug.
2. **`VF_RENDER_FLAGS=255` is not a bit-1 escape hatch** — it also has to clear
   bit 8 for `aoShEnclosure` to return 0. Before the bit-8 term existed, the
   proxy kept darkening shadowed+occluded surfaces and a cave A/B came back
   identical either way (45.26 on vs off), which is how the no-op was found.
3. The sun march itself is gated on the same bit in `common_svo.glsl:583` and
   `common_splat.glsl:247`.

## The honest cost picture for a per-frame sun march

| path | sun occlusion | per fragment? |
|---|---|---|
| splat opaque | baked | no march |
| splat water | `softShadowSplat` — 16-tap binary march of `heightAt` + `objDist`, 20 m | yes, already |
| SVO | `softShadow` — 16-tap PCF over `exactSVOHit` | yes (the reference) |
| SVO submerged bed | `softShadowTerrain` — 28-tap heightfield-only | yes |

The splat water path already pays a per-pixel march, which makes it the
instrument for measuring the other one rather than extrapolating from a guess.
The binding constraint on any splat sun march is field resolution, not taps:
`objDist` is the coarse `r8_snorm` 256³ volume at 0.4 m texels clamped to
±1.26 m, and an empty read returns exactly `+kObjVolMax` — beyond that range it
carries no information at all. Do not use it for shading normals, and do not
expect a contact shadow from it.

Cross-links: [[concepts/dynamic-sun-shadows]], [[concepts/sun-direction-pipeline]],
[[concepts/enclosed-space-lighting]], [[concepts/live-edit-surfel-parity]],
[[concepts/detail-pipeline]], [[concepts/measurement-discipline]].