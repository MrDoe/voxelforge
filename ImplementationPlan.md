# Voxelforge — Implementation Plan & Status

**Vision:** A real-time voxel renderer that produces a *photorealistic natural landscape* —
river valley with a water stream featuring reflections, foliage and grass, realistic lighting,
and a log cabin — built on a fully dynamic chunked sparse-voxel-octree architecture, with a
Gaussian-splat hybrid path planned on top.

Last updated: 2026-08-23

---

## 1. Architecture (as built)

```
┌───────────┐   ┌──────────────────────────────────────────────┐
│ GLFW/ImGui│   │ Vulkan 1.3 RHI (VMA, timeline-ready)         │
│ HUD/input │   │ compute pipelines + 1 graphics pipeline      │
└─────┬─────┘   └───────────────┬──────────────────────────────┘
      │                         │
┌─────▼─────────────────────────▼──────────────────────────────┐
│ World model (src/voxel)                                      │
│   common.hpp   shared deterministic scene fn (CPU truth)     │
│   world.cpp    16³ chunk grid → per-chunk octrees → 8³ bricks│
│   volume.cpp   dense 256³ reference region                   │
├──────────────────────────────────────────────────────────────┤
│ Render passes (src/render)                                   │
│   RaymarchPass  dense SDF sphere-tracer (reference backend)  │
│   SvoPass       chunked-SVO traversal (primary backend)      │
│   SplatPass     surface point-sprites (F-toggle preview)     │
├──────────────────────────────────────────────────────────────┤
│ Physics: Jolt (planned M6) · Splats+shadows: impl. plan §M7  │
└──────────────────────────────────────────────────────────────┘
```

Key properties: fully dynamic world model (edit-ready), two interchangeable rendering
backends validated against each other by an automated image-diff test (`--compare`),
headless self-tests for CI-style verification.

---

## 2. Implemented & verified ✅

### M0 — Foundation ✅
- [x] Vulkan instance/device/queues, VMA allocator, swapchain (per-image acquire semaphores)
- [x] GLFW window + fly camera (WASD/QE + RMB look + wheel speed)
- [x] ImGui HUD (GPU/fps/cam/mode/backend panels)
- [x] Shader build pipeline (glslang → SPIR-V at compile time)

### M1 — Dense debug volume ✅
- [x] 256³ SDF sphere-tracing compute shader (normals via gradient, soft shadows,
      hemispheric ambient, fog, sky model)
- [x] snorm8 distance encoding shared CPU↔GPU

### M2 — Chunked SVO world ✅ (the core milestone)
- [x] `World::build()` — multithreaded per-chunk SVO builder with conservative
      empty/solid cell classification (margin-inflated for non-Lipschitz banks),
      solid-subtree collapsing, 8³ brick emission (packed color + int8 voxel-unit SDF)
- [x] GPU traversal shader: chunk grid → octree descent → manual trilinear brick
      sampling; relaxed step for super-Lipschitz slopes
- [x] Deterministic pool merge (node/handle/brick offset fixups)
- [x] Backend switch: `--backend svo|dense` (SVO default)
- [x] Current scene stats: ~30k nodes / 110k bricks / 563 of 4096 chunks active / 216 MB /
      builds in ~6 s (with hut+pines detail)

### Rendering features in place
- [x] Sun soft shadows (screen-space penumbra march), hemispheric sky light,
      ground bounce, distance fog
- [x] **Water**: analytic plane y=-0.9 clipped to channel mask; ripple normals;
      Fresnel; planar-reflection secondary march; sun glint; underwater absorption tint
      *(SVO backend only — see §4 debt)*
- [x] **ACES tonemapping** *(SVO backend only)*
- [x] Grass micro-detail (normal jitter + albedo variation)
- [x] **Log cabin**: stacked-cylinder walls ×7 courses, gable roof, door CSG cutout,
      flattened terrain pad
- [x] **Pines ×4** (trunk capsule + 3 cone canopy layers, height-varied)
- [x] **Terrain v2**: meandering stream channel (Lipschitz-safe banks), layered
      materials (grass variation / wet sand / soil band / rock bed), boulders ×3
- [x] Gaussian-splat **preview layer**: surface point-sprites w/ isotropic Gaussian
      falloff, premultiplied blending, F-key toggle, derived automatically from any backend

### Tooling & tests ✅
| Suite | Status |
|---|---|
| Unit tests (doctest): builder determinism/pruning, point-query vs analytic SDF,
dense volume, snorm8 roundtrip, camera math, worldfile roundtrip/corruption | ✅ 16/16 cases |
| GPU selftests (`--selftest`) SVO / dense / splat | ✅ PASS |
| Backend equivalence (`--compare`): mean channel diff 4.32/255 (heightmap normals +
shared heightfield shadows + 640-step budget), coverage parity | ✅ PASS |
| Smoke runs (headless frame loops) | ✅ 0.04–0.19 ms/frame |
| Interactive stability | ✅ ~150 fps, no stalls (post-fix) |

**World asset pipeline (2026-08-23):** `tools/heightmap_gen` generates the 16-bit
heightmap PNG **and** builds the real chunked-SVO world from it, then serializes
`assets/world.vxw` (**VXW v1**): header (meta + CRC32) → GPU SVO buffers with
**2 words per voxel** (`rgb+sdf` | `a+refl+rough+matId`, see `kMaterialReflection`)
→ explicit surface-band voxel records (u16³ grid pos, RGBA, reflectivity, roughness,
material). The app loads the file directly at startup — no CPU build on the boot path
(262 MB GPU buffers + 4.1 M surface records) — and falls back to procedural
`World::build()` if the file is missing/stale. Splats derive straight from the records.
Generation remains heightmap-driven end-to-end; splats are an alternative rendering
mode only.

Hard-won fixes baked in: null pipeline layout (NVIDIA compiler segfault), NVIDIA/X11
MAILBOX present deadlock (→ IMMEDIATE default + per-image acquire semaphores), NDC flip
in sprite path, stale mouse deltas, solid-chunk handle `-2` misread as empty chunk,
deterministic merge offsets, late backend resolution, failure-path cleanup segfaults.

**Session fix (2026-08-23) — SVO rendered only a 10 m sphere:** the SVO `map()` in
`shaders/svo_raymarch.comp` had been left as a debug stub `return length(p) - 10.0;`
("DBG2"), so every ray marched a sphere of radius 10 around the origin — the whole
landscape was invisible. Replaced with the real chunked-SVO traversal (chunk grid →
per-chunk octree → 8³ bricks, mirroring `World::sample`). Also fixed the brick SDF
decode in `brickVoxelSdf`: it used `raw/127 * pc.b.y` which clamped distances to ±0.1 m;
correct is `raw * pc.b.y` (the brick stores `sdf/VOXEL`, so decode is `sdf = raw*VOXEL`).
After both fixes the SVO renders the full valley and `--compare` passes
(mean diff 27.14/255, coverage 50.0 % vs 47.9 %).

**Reference-volume fix:** `DenseVolume` was a 25.6 m patch (256³ @ 0.1 m) centred at the
origin while the SVO world spans the full 102.4 m — so the two backends rendered entirely
different scenes and `--compare` could never agree. `vol.worldSize` is now set to
`WORLD` (same 256³ voxel count, just 0.4 m over the full span), giving a fair parity test.

---

## 3. Known issues / technical debt ⚠️

1. ~~Dense backend lacks water + ACES → `--compare` fails~~ **RESOLVED**: the dense
   `raymarch.comp` now shares the SVO shading model (ACES, analytic water plane with
   planar reflection, grass micro-detail, underwater tint, hemispheric sky + ground bounce),
   so both backends produce identical photorealistic output and `--compare` is green again.
2. **Point-query agreement ~37 %** near thin features (logs, trunks, canopy): cell-solid
   shortcuts trade exactness for speed where features are thinner than classification
   margins. Renderer output unaffected (image tests pass); test threshold documented.
3. **World build time grew to ~6 s** (CPU) with detailed content → motivates M3 GPU builds.
4. **VRAM 216 MB** for one landscape — fine now, DAG dedup will shrink it.
5. No validation layers installed on this machine (LunarG SDK tarball needed) — errors are
   caught by custom checks + image tests instead.
6. Screenshot loop (`--shot`) works but warmup frames are slow with reflections enabled.
   Final shots land in `tests/screenshots/` (`m2_valley.png`, `m2_stream.png`, `m2_hut.png`,
   `cmp_svo.png`, `cmp_dense.png`).
7. **Copilot share link** `https://copilot.microsoft.com/shares/tasks/GXMVnBaX1diUAkq41Bbeq`
   was a client-rendered SPA behind an auth wall (HTTP 460 on the data API) and could not be
   fetched headlessly; the user pasted its full text. It is the report *"Physically-Based
   Billboard Splats in a Fixed 3D Grid"* and is woven into §5 below (and folded into P4/P1/P2).

---

## 4. Roadmap — what still needs to be done

### Phase P0 — finish photorealism batch
- [x] **SVO renders the full valley** (sphere-stub `map()` + brick decode fixed) and matches
      the dense reference → `--compare` green (27.14/255, coverage parity)
- [x] Duplicate water + ACES into dense backend → backend parity restored
- [ ] Visual iteration pass on screenshots in `tests/screenshots/` (`m2_valley`, `m2_stream`,
      `m2_hut`, `cmp_svo`/`cmp_dense`): tune water mask edges (channel lim), hut proportions,
      sun angle/warmth
- [x] **Sun control**: `--sun <elev> <azim>` (degrees) threads a normalized sun direction
      through `RaymarchPush` (now 112 B) into both SVO + dense shaders; default (56.5°,54.5°)
      reproduces the prior sun exactly → `--compare`/`--selftest` stay green
- [x] **Ripple animation fixed**: water ripple phase now reads `pc.b.w` (frameIdx) instead of
      `pc.a.w` (extentY), so ripples animate in live (non-headless) mode
- [ ] Water flow polish: foam lines at banks, depth-based color gradient
- [ ] Exposure/saturation grading pass
- [ ] More pines + deciduous variant; scattered rocks/grass tufts as instanced details

### Phase P1 — realism upgrades (was M5)
- [ ] Sky model (Hosek-Wilkie or Preetham) replacing gradient sky
- [ ] 1-bounce diffuse GI: half-res temporal accumulation + variance-guided filter
- [ ] TAA with reprojection from depth/normal history
- [ ] PBR-ish material model per palette entry (roughness/metallic/emissive bytes in brick)

### Phase P2 — dynamic world (was M3/M4)
- [ ] GPU-side chunk builds off a dirty-chunk queue (editing groundwork)
- [ ] CSG brush editing (sphere/box/noise stamps), incremental re-octalization
- [ ] DAG dedup of cold chunks (Teardown-style subtree merging; big VRAM win)
- [ ] Pool budgets + LRU eviction with host-side compressed mirror
- [ ] Undo stack (before-images of dirty chunks)

### Phase P3 — physics & gameplay (was M6)
- [ ] Jolt integration; SDF narrowphase sampled from GPU pools (1-frame latency cache)
- [ ] Character controller + rigid debris interacting with water buoyancy

### Phase P4 — Gaussian-splat path (was M7, per `implementation_plan.md`)
- [ ] Derive anisotropic Gaussians fitted to brick surfaces (replacing point sprites)
- [ ] Tile-sort + blended splat pass sharing the offscreen/blit pipeline
- [ ] **Minimal-raytrace shadows per `implementation_plan.md`:** one visibility ray per
      splat against our SVO proxy (no shadow maps); binary factor → smoothstep softening
- [ ] F-key toggle becomes true representation switch; crossfade blend
- [ ] NSVF-inspired extension (M8): per-brick feature vectors decoded by tiny in-shader
      MLP → view-dependent appearance + splat-parameter generation
- [ ] **Physically-based splats (see §5):** per-splat `σ_t` + Henyey–Greenstein `g`;
      Beer–Lambert front-to-back compositing; **Weighted Blended OIT** to kill pop-in;
      **ray-marched water/fog volume** reusing SVO empty-skip; octree LOD + splat budget +
      8-bit texture quantization for scale.

### Phase P5 — scale & polish (stretch)
- [ ] Out-of-core streaming of cold chunks
- [ ] Multi-light support, temporal smoothing (per implementation_plan.md extensions)
- [ ] Benchmark scene + profiling harness (Tracy wired but unused)

---

## 5. External Plan — Physically‑Based Billboard Splats in a Fixed 3D Grid

Source: Copilot share `GXMVnBaX1diUAkq41Bbeq` (user‑pasted; headless fetch blocked by auth).
This is a theory/report document; below is the **actionable distillate** mapped onto Voxelforge's
existing splat layer (current point‑sprite preview) and the M7 minimal‑raytrace guide
(`implementation_plan.md`). Where it diverges from our fast pragmatic path, both are noted.

### 5.1 Splat material model (per‑atomic‑unit)
Treat each splat as a volumetric primitive carrying:
`position μ`, `scale s`, `rotation/quaternion r` (or covariance Σ for anisotropy),
`albedo a`, `extinction σ_t = σ_a + σ_s`, `scattering σ_s`, `alpha/transparency`, optional
`emission`, and a `phase function p(θ)` (isotropic / Rayleigh / Mie / Henyey–Greenstein).
Billboard flavour adds an `RGB texture` + `alpha map` for high‑frequency shape/soft edges.
*Voxelforge mapping:* today's splats are isotropic point sprites with a Gaussian falloff and a
single albedo. Upgrade path = attach per‑splat `σ_t` + `phase` so water/fog/boulder media shade
physically instead of via bespoke shader hacks.

### 5.2 Light‑transport math (the core equations to adopt)
- **Beer–Lambert transmittance:** `T(d) = exp(−∫ σ_t(x) dx)`.
- **Discrete splat integration** along a ray (front‑to‑back):
  `L = Σ_i c_i · (1 − exp(−σ_i·δ_i)) · Π_{j<i} exp(−σ_j·δ_j)`.
  This is exactly front‑to‑back volumetric compositing and should replace ad‑hoc alpha blending
  for any participating medium.
- **Volume rendering equation** (in‑scattering via phase function + emission) for fog/water.
- **Numerical stability:** early‑terminate when `T` < ε; adaptive step size in high‑σ regions;
  Russian‑roulette / ratio‑tracking for unbiased Monte‑Carlo when we go stochastic.

### 5.3 Rendering pipelines — pick per effect
- **Rasterize billboards** (textured quads) for surface‑like splats — what we already do.
- **Ray‑march the grid** for true volumetrics (fog, water body, smoke) — shares the SVO/empty‑skip
  traversal we already built for the SVO backend.
- **Hybrid:** mesh/SVO surfaces via rasterization + participating media via ray‑march. Our SVO
  backend is the natural proxy for the "volume" leg.

### 5.4 Order‑independent transparency for dense clouds
`Weighted Blended OIT` (cheap, approximate) or `per‑pixel linked lists` (accurate, VRAM‑heavy)
or `Layered OIT` (fixed N layers). `Stop‑the‑Pop` smooths sort order changes. Our current
point‑sprite blend is order‑dependent and will pop once splat counts/clouds grow → adopt
Weighted Blended OIT first.

### 5.5 LOD / streaming / compression (scaling to big grids)
- **Spatial LOD** via octree partitioning; importance‑based pruning (drop low‑opacity/low‑volume
  splats at distance).
- **Streamed SOG** (sorted‑octree‑grid) loads LODs by camera distance; **global splat budget**
  caps active count.
- **Compression:** 8‑bit texture quantization (4×+), sparse/empty‑cell skipping, dictionary
  coding. Mirrors the SVO DAG‑dedup idea (P2) but at the splat tier.

### 5.6 Phase functions & material case studies (directly relevant to our valley)
- **Stone/boulders:** high σ_t, subsurface diffusion (soft colour bleed) — our 3 boulders.
- **Water:** depth‑varying absorption (long wavelengths first), strong **forward** scattering
  (HG with high `g`), air/water Fresnel + refraction at the surface splats; the existing analytic
  water plane (P0) can be superseded/augmented by a splat‑volume water body using 5.2.
- **Fog/atmosphere:** HG (high `g`) + light shafts from volumetric shadows (crepuscular rays) —
  pairs with the sky/sun model (P1).
- **Glass:** transmission + refraction via per‑splat normals + IOR; secondary rays for caustics.

### 5.7 Physics coupling (future)
Fixed grid aligns with **SPH** (`SPlisHSPlasH`) and Eulerian fluids; per‑splat material →
sim parameters; temporal densify/prune keeps frame‑to‑frame coherence. Relevant once P2 dynamic
world lands.

### 5.8 Reconciliation with our M7 minimal‑raytrace guide
`implementation_plan.md` specifies **one visibility ray per splat** against a proxy (SVO/BVH/grid)
+ `color *= (1 − shadowFactor)` — a deliberately fast, stable shadow approximation. Keep that as
the **default shadow path**; layer the §5.2 volumetric model only for participating media
(water/fog), not for opaque‑splat self‑shadowing. Both coexist: proxy‑ray shadows for splats,
ray‑marched transmittance for volumes.

### 5.9 Folded‑in backlog (add to roadmap)
- [ ] Per‑splat `σ_t` + `phase` (HG `g`) material fields; Beer–Lambert compositing for splats.
- [ ] Switch splat blend to **Weighted Blended OIT** (kills pop‑in at scale).
- [ ] **Ray‑marched water/fog volume** reusing SVO empty‑skip; HG phase for forward scatter.
- [ ] **LOD/streaming**: octree splat LOD + global splat budget + 8‑bit texture quantization.
- [ ] (Later) SPH/fluid coupling once P2 dynamic world exists.

---

## 6. Commands

```bash
./build/voxelforge                          # interactive SVO landscape
./build/voxelforge --backend dense          # dense reference region
./build/voxelforge --mode splat             # splat preview (or F to toggle)
./build/voxelforge --selftest               # GPU regression check (exit code)
./build/voxelforge --compare                # SVO↔dense image equivalence
./build/voxelforge --smoke 400              # headless stability run
./build/voxelforge --sun 56.5 54.5          # sun elevation°/azimuth° (default)
./build/voxelforge --shot out.ppm \
  --cam X Y Z TX TY TZ                      # screenshot at arbitrary camera
ctest --test-dir build                      # unit tests
```
