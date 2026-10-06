---
title: Sun Direction Pipeline (kSunDir) — Production & Consumers
tags: [sun, lighting, sky, shadows, day-night, kSunDir]
sourceRefs:
  - src/app/cli/args.hpp
  - src/app/cli/args.cpp
  - src/app/frame/run.cpp
  - src/voxel/worldfile.cpp
  - src/voxel/worldfile.hpp
  - src/voxel/surfelize.cpp
  - src/render/svo_pass.hpp
  - shaders/common_base.glsl
  - shaders/common_svo.glsl
  - shaders/common_splat.glsl
  - shaders/svo_raymarch.comp
  - shaders/ssr.comp
  - shaders/volumetric_fog.comp
lastReviewed: 2026-10-06
---

# Sun Direction Pipeline (`kSunDir`)

How the sun direction is produced, uploaded to the shader, and every place it
consumes it. This is the map to read before touching day/night, time-of-day, or
any sun-related shader work.

## Production (CPU side)

### Input sources (priority order)

1. **CLI `--sun <elev> <azim>`** — highest priority when present.
   Parsed in `src/app/cli/args.cpp` into `args.sunElev` / `args.sunAzim` /
   `args.sunSet`. Default: 34° / 238° (golden hour).

2. **Manifest `"sun": {elev, azim}` block** in `world.json` — used when
   `--sun` is NOT given. Parsed by `worldfile::loadSunManifest`
   (`src/voxel/worldfile.cpp:649`). Non-finite values fall back to 34/238.
   The `worldfile.hpp:152` comment says *"the shader derives the moon from it"*
   but **no moon/night code exists in `shaders/`** — that comment overstates.
   **Trap-and-fix (resolved):** `loadSunManifest` used to return `true` on a
   half-written `"sun"` block, leaving the missing key at `0.000` — the
   `isfinite` guard never caught the caller's 0 seed, so `{"sun":{"elev":12}}`
   parked the sun due north (azim 0) while the log read as healthy. Fixed by
   requiring `gotElev && gotAzim`; returns `false` and warns on a half-written
   block. Pinned by `tests/test_worldfile.cpp:785`. See
   [[concepts/sky-probe-is-a-camera-assertion]] for the full trap-and-fix story.

3. **Hardcoded default** — 34° / 238° when neither source is present.

### Direction computation (`App::run`, `src/app/frame/run.cpp:48-60`)

```cpp
const float er = glm::radians(e), ar = glm::radians(a);
m_sunDir = glm::vec4(
    glm::normalize(glm::vec3(cosf(er) * sinf(ar), sinf(er), cosf(er) * cosf(ar))), 0.0f);
```

`m_sunDir` is a `glm::vec4` member of `App`. The direction is **toward** the
sun. Elevation below the horizon is valid (night).

### Animation clock model

`m_animTime` (`float`, seconds) advances **only interactively** — headless
`--shot`/`--selftest` runs keep it frozen for determinism. It feeds
`pc.misc.y` (cloud drift, wind/grass). A time-of-day clock for day/night
should follow the same pattern: advance only when `!fx.headlessRun`.

## Upload to shader

The sun direction reaches the shader through the **push constant** `sunDir`
field, present in every pipeline's PC struct:

| Pipeline | PC struct | File |
|----------|-----------|------|
| SVO raymarch | `RaymarchPush` | `src/render/svo_pass.hpp:13` |
| Splat vertex | anonymous PC | `shaders/splat.vert` |
| SVO compute | anonymous PC | `shaders/svo_raymarch.comp` |
| SSR | anonymous PC | `shaders/ssr.comp` |
| Volumetric fog | anonymous PC | `shaders/volumetric_fog.comp` |

The shader global `kSunDir` is set from `pc.sunDir` in `main()`. Exception:
`ssr.comp` has its own `skyColor()` that reads `normalize(pc.sunDir.xyz)`
directly (it does not include `common_base.glsl`).

## Consumers

### 1. Sky rendering (`shaders/common_base.glsl`)

- **`skyColor(vec3 d)`** — full Preetham/Hosek-inspired sky with procedural
  clouds, golden-hour warmth, sun disc + glow. Uses `kSunDir` for:
  - `cosGamma = dot(d, kSunDir)` — sun angle for disc/glow
  - `lowSun = 1 - smoothstep(0.08, 0.55, kSunDir.y)` — golden-hour blend
  - cloud cover modulation
- **`skyColorFast(vec3 d)`** — cloud-free variant for indirect/irradiance/fog.
  Same `kSunDir` usage minus clouds.

### 2. Terrain shading (`shaders/common_svo.glsl`)

- **`shadeTerrain()`** — primary surface shading. Uses `kSunDir` for:
  - `softShadow(p + n*0.35, kSunDir)` — shadow march
  - `ndl = max(dot(n, kSunDir), 0)` — direct light
  - half-vector `h = normalize(kSunDir + V)` — specular
  - foliage backlit translucency
  - subsurface scattering
  - refracted-sun caustics on water bed
- **`shadeFloor()`** — submerged terrain bed (reflected by water). Uses
  `kSunDir` for `softShadowTerrain`, `ndl`, specular.

### 3. Splat shading (`shaders/common_splat.glsl`)

- **`shadeSurfel()`** — exact twin of `shadeTerrain` but with **baked** shadow
  + AO (zero march cost per fragment). Still uses `kSunDir` for `ndl`,
  half-vector, foliage, SSS, caustics.

### 4. Shadow functions

| Function | Backend | File | Method |
|----------|---------|------|--------|
| `softShadow(ro, rd)` | SVO | `common_svo.glsl` | Binary DDA hit (`exactSVOHit`) |
| `softShadowSplat(ro, rd)` | Splat | `common_splat.glsl` | 16-tap binary march (heightfield + objVol) |
| `softShadowTerrain(ro, rd)` | Shared | `common_base.glsl` | Heightfield-only 9*s/t kernel |

All take `kSunDir` as `rd`.

### 5. CPU surfel bake (`src/voxel/surfelize.cpp`)

- **`shadowMarch(field, ro, rd)`** (line 419) — two-phase march: cell-exact
  DDA to 3 m, then sphere-tracing to 60 m. Called with `sunDir` from
  `params.sunDir`.
- **`shadeCandidates()`** (line 2033) — live-edit store path. Uses
  `params.sunDir` for shadow + AO bake.
- **`buildSurfels` pass 2** (line 828) — full bake. Same `params.sunDir` usage.

The baked shadow verdict is stored in `Surfel::bent_sh.w` and the AO in
`Surfel::mat_ao.w` (packed with owner metadata).

### 6. Water

- **`svo_raymarch.comp`** — water glint (`pow(max(dot(reflect(rd,n), kSunDir),0), 420)`),
  caustics, shore foam. Uses `kSunDir` directly.
- **`shadeWaterSplat`** in `common_splat.glsl` — same formula for the splat backend.

### 7. Fog

- **`fogColor(rd, p)`** (`common_base.glsl`) — sun-warmed aerial perspective.
  Uses `kSunDir` for `sunAmt` and `lowSun`.
- **`volumetric_fog.comp`** — Mie in-scatter with `pc.sunDir` for the forward lobe.

### 8. Sky irradiance

- **`skyIrradiance(n)`** (`common_base.glsl`) — 3-tap analytic ambient.
  Tap `b = normalize(n*0.7 + kSunDir*0.3)` biases toward the sun.

### 9. SSR

- **`ssr.comp`** — has its own `skyColor()` (duplicate of the fast variant)
  using `normalize(pc.sunDir.xyz)`. Does NOT use the `kSunDir` global.

## Reference-shot gate warning

> **A `"sun"` key in `world.json` silently changes EVERY reference shot's
> lighting.** It moves `visual_check` coverage, black-in-silhouette numbers,
> and all baseline comparisons. The gates that render reference shots
> (`visual_check.py`, `live_edit_check.py`, `fog_check.py`) should pin `--sun`
> explicitly so a manifest edit does not cause false failures.

## Day/night implementation notes

For the day/night cycle work (George, shading engineer):

- **No moon/night code exists.** `grep -ri moon shaders/` = 0 hits. The
  `worldfile.hpp:152` comment is aspirational.
- `skyColor()` has a `lowSun` term but no night path — no stars, no moon disc,
  no dark sky gradient.
- The `m_animTime` pattern (advance only interactively) is the model for a
  time-of-day clock.
- All consumers listed above read `kSunDir` (or `pc.sunDir`), so a day/night
  system that varies the sun direction + adds a night sky branch in
  `skyColor()` will propagate everywhere automatically.
- The CPU bake (`surfelize.cpp`) uses `params.sunDir` — if the sun moves, the
  baked shadows/AO are stale until the next `rebuildSurfels()`. A day/night
  cycle needs either a live re-bake strategy or acceptance that baked shadows
  are "frozen" at the bake-time sun.
