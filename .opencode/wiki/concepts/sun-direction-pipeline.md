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
   The `worldfile.hpp:152` comment about the shader deriving the moon is now
   accurate — see the night lane below.
   **Trap-and-fix (resolved):** `loadSunManifest` used to return `true` on a
   half-written `"sun"` block, leaving the missing key at `0.000` — the
   `isfinite` guard never caught the caller's 0 seed, so `{"sun":{"elev":12}}`
   parked the sun due north (azim 0) while the log read as healthy. Fixed by
   requiring `gotElev && gotAzim`; returns `false` and warns on a half-written
   block. Pinned by `tests/test_worldfile.cpp:785`. See
   [[concepts/sky-probe-is-a-camera-assertion]] for the full trap-and-fix story.

3. **Hardcoded default** — 34° / 238° when neither source is present.

**Provenance note (2026-10-06):** `assets/world.json` is **sunless** — its keys
are `layers` / `textures` / `version` only. No `"sun"` key was authored, so
every reference shot still renders at the CLI default 34/238 and their coverage
/ black-in-silhouette numbers are unchanged. The A/B arms measured below
therefore came from **CLI `--sun` flags**, not from a manifest edit — nothing
about the shipped scene moved.

### Night lane (landed 2026-10-06)

The night path is real and lives in `shaders/common_base.glsl`:
`nightFactor()` (526), `sunFade()` (533), `moonDir()` (546), `kMoonCol` (551),
`moonLight()` (557), `applyNight()` (569). `applyNight` is called from **both**
sky variants (`skyColor` 646, `skyColorFast` 679) and `moonLight` reaches both
splat paths, so `worldfile.hpp:152` and `test_worldfile.cpp:804` are accurate
about the moon being derived from the sun.

> **Caution — do not "restore" `kMoonCol = 0.62`.** That value was tried first
> and produced mean luma 46, which read as dusk rather than night.
> **0.14 is the settled value.**

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

### Measured (640×360, hero cam `1.0 2.0 1.5 → 5.3 1.0 11.3`, splat)

Reported by the day/night session, rendered on that session's tree:

| Arm | Manifest | mean luma | dark% | blue% |
|-----|----------|-----------|-------|-------|
| (a) null control | no `"sun"` key → CLI default 34/238 | 120.43 | 0.01 | 32.1 |
| (b) low sun | `{"sun":{"elev":4,"azim":240}}` | 100.60 | 0.03 | 46.1 |
| (c) night | `{"sun":{"elev":-30,"azim":96}}` | 32.92 | 23.49 | 56.7 |

Night-vs-day colour: sky RGB `(12.5, 23.4, 37.0)` vs `(127.8, 137.9, 133.9)`;
ground `(35.5, 47.5, 43.6)` vs `(124.6, 130.1, 106.9)`.

**How to read this.** Arm (a) is a null control, not a data point: 34/238 is the
CLI default, so an absent key must reproduce the current reference shots
exactly. Arms (b) and (c) are the real measurement — dark% goes 0.01 → 0.03 →
23.49, i.e. **a below-horizon sun pushes the frame past the `< 5 %`
black-in-silhouette gate by ~4.7×**. Any gate that renders a night scene
without pinning the sun will fail for this reason alone.

Caveat: these are **George-reported**, not measured by the wiki session.
Numbers only re-derive within one tree — mixing arms from different sessions
inherits that tree's content drift. Re-run the null control as a measured arm
before quoting a delta. See [[concepts/sky-probe-is-a-camera-assertion]] for
the companion `b >= r` measurement on the same arms.

Also pinned by the day/night work: `tests/test_worldfile.cpp` fixes
`{"sun":{"elev":-14,"azim":96}}` as a **valid night** — elevation below the
horizon is deliberately NOT clamped.

### Day/night switch (in the Render panel)

A discrete **Day / Night** toggle sits in the Sun section above the
elevation/azimuth sliders, which stay as the fine control underneath it. It
snaps `m_sunDir` between two fixed angles:

- **Day** — elev 34°, azim 238° (the golden hour the gates are calibrated to)
- **Night** — elev −30°, azim 96° (the measured night arm)

Hotkey **P**. Both snap points are the arms already measured in the table
above, so the toggle's two states are numbers that exist rather than guesses.

`setSunPhase()` calls `requestWorldReload()`, and **that rebake is a
deliberate stall, not an async handoff.** The SVO rebuild itself is threaded
(`LayeredWorld::kick` spawns a worker unless `VF_SYNC_RELOAD`), but the frame
that *applies* it — `App::applyWorldReload` — calls `vkDeviceWaitIdle` and then
`rebuildSurfels()`, and the per-surfel sun shadow is CPU-baked at surfelize
time. So a toggle costs a visible hitch of roughly the same order as the `U`
micro-detail toggle.

Do not read "the reload is queued" as "the shadows will catch up on their own" —
nothing chases it. The sky and direct light move on the next frame because
`m_sunDir` rides the push constant; the baked shadows do not move until the
rebake finishes.

### Proving both arms headlessly

`VF_TEST_SUN_PHASE=day|night` drives the **same `setSunPhase()` path as the
buttons**, so neither arm requires clicking the GUI:

```
VF_TEST_SUN_PHASE=night ./build/voxelforge --shot night.ppm --cam ...
```

Use it instead of a manual click — and note that `--shot` **cannot** prove the
buttons work at all: `drawHud()` runs only from `record_interactive.cpp`, so no
headless render ever draws the sidebar. That is the standing gap in
[[concepts/interactive-ui-coverage-gap]], and it applies here unchanged: the
switch's *rendered result* is headlessly provable, the switch's *UI* is
manually verified only.

### The switch deliberately does NOT persist

> **"The switch does not save your night" is a feature, not a gap.**

Writing the phase back to `assets/world.json` was rejected, and the reason is
today's incident on that exact file: a pre-existing uncommitted edit by an
unknown session was destroyed by a stray `git checkout --` and recovered only
via a scratch copy plus md5 reconstruction (md5 `dac9f059…`, still ` M` — see
[[concepts/uncommitted-edit-is-not-yours]]). A GUI that rewrites a tracked
manifest on every toggle would put that same file back in the blast radius of
routine interaction.

`assets/world.json` therefore **still has no `"sun"` key**, which is what keeps
every reference shot at 34/238 with unchanged coverage and
black-in-silhouette numbers. Persisting a night scene means editing the
manifest deliberately, not clicking a button.

### Which gate actually catches a day/night change

The two `visual_check` assertions split cleanly, and the split is the finding:

- **The sky probe (`b >= r`) cannot detect day/night at all.** The blue share
  *rises* across the arms — 32.1 % → 46.1 % → **56.7 %** — so the probe passes
  comfortably at `elev -30`. The reason is structural: `b >= r` is
  brightness-independent by construction and only tests for *warmth*. Night sky
  is `(12.5, 23.4, 37.0)`, which satisfies `b >= r` as strongly as day
  `(127.8, 137.9, 133.9)`, which satisfies it by 6 codes. A 34° → −30° sun
  change is invisible to this assertion.
- **Black-in-silhouette is the assertion that moves**: 0.01 % → 23.49 %, past
  the `< 5 %` gate by ~4.7×.

So a night scene fails `visual_check` on **silhouette, not sky** — and a gate
pinned only on the sky probe will report a night frame as healthy. This is the
strongest form of the claim in [[concepts/sky-probe-is-a-camera-assertion]]:
the probe is not merely a camera assertion on those three shots, it cannot
detect the very transition a day/night lane introduces. Pin the sun (or use a
day-only fixture) for both assertions.

### Consequence: the night verdict is currently an argument, not a measurement

If a night frame fails on silhouette and not on sky, then `test-visual`'s night
verdict is an **argument between two gates** rather than a measurement — and
silhouette is the wrong gate for it. 23.49 % dark pixels in a legitimately dark
scene is not a rendering defect; it is the time of day. `black_in_obj > 0.05`
therefore fails **every** night scene by construction, and whoever authors one
will read it as "my lighting is broken" and start tuning brightness instead of
fixing the gate — the same trap as the sky probe, one level down: an assertion
calibrated on daylight content being read as a shading verdict.

**The day/night lane needs its own reference arms** — night shots with
night-appropriate budgets — not the existing daylight thresholds reinterpreted.
Until those exist, treat a night `visual_check` failure as a missing-fixture
signal, never as a lighting defect. See
[[concepts/sky-probe-is-a-camera-assertion]] for the other two
assertion-is-wrong cases.

## Day/night implementation notes

For the day/night cycle work (George, shading engineer):

- **The night lane has landed** — see the Night lane section above for the
  real function set. `moonLight()` reaches both splat paths, and the SVO twin
  adds `+ moonLight(n, 1.0)` to its ambient.
- The `m_animTime` pattern (advance only interactively) is the model for a
  time-of-day clock.
- All consumers listed above read `kSunDir` (or `pc.sunDir`), so a day/night
  system that varies the sun direction + adds a night sky branch in
  `skyColor()` will propagate everywhere automatically.
- The CPU bake (`surfelize.cpp`) uses `params.sunDir` — if the sun moves, the
  baked shadows/AO are stale until the next `rebuildSurfels()`. A day/night
  cycle needs either a live re-bake strategy or acceptance that baked shadows
  are "frozen" at the bake-time sun.
