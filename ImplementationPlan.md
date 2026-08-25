# Voxelforge — Implementation Plan & Status

**Vision:** A real-time voxel renderer that produces a *photorealistic natural landscape* —
river valley with a water stream featuring reflections, foliage and grass, realistic lighting,
and a log cabin — built on a fully dynamic chunked sparse-voxel-octree architecture, with a
Gaussian-splat hybrid path planned on top.

Last updated: 2026-08-24

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
| Backend equivalence (`--compare`): measured 7.37/255, thresholds tightened to
  <14/255 + <8% coverage delta (was a lenient 32/255 that hid real divergence) | ✅ PASS |
| Visual regression (`tests/visual_check.py`, ctest `visual_check`): canonical shots
  assert black-in-silhouette <5%, coverage range, blue sky probe | ✅ PASS |
| Smoke runs (headless frame loops) | ✅ 0.04–0.19 ms/frame |
| Interactive stability | ✅ ~150 fps, no stalls (post-fix) |

**World asset pipeline (2026-08-23, on-demand):** `tools/heightmap_gen` is the **once on-demand**
terrain generator (`ninja world` or `heightmap_gen assets/heightmap.png assets/world.vxw`).
It builds the 16-bit heightmap PNG, then the full chunked-SVO world from the
procedural scene (terrain + house/trees/rocks/bushes) and serializes the **single**
`assets/world.vxw` (**VXW v1**): header (meta + CRC32) → GPU SVO buffers with
**2 words per voxel** (`rgb+sdf` | `a+refl+rough+matId`, see `kMaterialReflection`)
→ explicit surface-band voxel records (u16³ grid pos, RGBA, reflectivity, roughness,
material, **unified scale for landscape + all objects**). The voxel file is the
**single source of truth** — the app loads it directly at startup (262 MB GPU
buffers + 4.4 M surface records) and uses it as the template for further editing;
no CPU build on the boot path, fallback to procedural only if the file is missing.
Splats are strictly derived from the voxel scene (lattice `scene()` SDF, `±0.22 m`
band, plus an 8-neighbour interpolated subgrid in splat mode for a much denser
`~1.5 M` splat set) — just another rasterization of the same voxels.

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
      the dense reference → `--compare` green (now 6.06/255 after heightmap normals +
      shared shadows + 640-step budget)
- [x] Duplicate water + ACES into dense backend → backend parity restored
- [x] **Heightfield scene** (`M8`): terrain from `assets/heightmap.png` (hills + carved,
      level-bed river); hut/pines/boulders deleted; materials from slope/shore bands
- [x] **Riverside objects, layered bottom-up**: log cabin on west bank (stone foundation →
      8 alternating log courses → door+windows carved → 10 stepped shingle layers →
      chimney) and broadleaf tree (root flare → tapered trunk → 4 foliage tiers);
      exact-primitive SDFs + analytic object normals in both backends; ground pads
      flattened in the generator; palette extended (wood/shingles/foliage)
- [x] **Constant-mist fix**: distance fog density 0.006 → 0.002 in both backends;
      terrain luminance spread at the canonical view widened 23 → 39 (p5 214→196)
- [x] **Object + terrain shadows**: softShadow marches min(heightfield, objectDistance)
      identically in both backends — house, trees and boulders cast soft shadows
      (sun-flip test: 28.9/255 directional response, zero pitch-black)
- [x] **Shoreline foam**: animated value-noise streaks hugging the waterline
      (depth-band masked; sparse by design)
- [x] **Grading**: post-ACES saturation lift (1.14) in both backends; sun glow
      strengthened so the light source reads in the sky
- [x] **Golden-hour defaults + hero shot**: sun defaults to 34°/238°; SVO spawn camera
      looks across the river at the cabin; splat scale keys (+/-) with HUD readout
- [x] **Valley scatter**: five additional trees + three half-buried bank boulders,
      all ground-hugging via sampled bases
- [ ] Visual iteration pass on screenshots in `tests/screenshots/` (fine-tuning)

### Phase P1 — realism upgrades (was M5)
- [x] Sky model (Hosek-Wilkie/Preetham via Perez distribution, turbidity 2.2) replacing gradient sky
- [ ] 1-bounce diffuse GI: half-res temporal accumulation + variance-guided filter
- [ ] TAA with reprojection from depth/normal history
- [x] PBR-ish material model per palette entry (roughness/reflectivity from second brick word via kMatRefl, Blinn-Phong specular in shadeTerrain)
- [x] **PBR pass 2 (2026-08-24):** GGX Trowbridge-Reitz D + Smith height-correlated V + Schlick F0 (per-material from refl) replacing Blinn-Phong, energy-conserving diffuse (1−Favg), Albedo-Scale multi-scatter compensation; multi-scale SDF AO (3 rings 0.14/0.45/1.25 m, bent normal) replacing the 4-dir voxelAO (and added to dense backend); sky-model-driven ambient via 3 tilted `skyColor` taps + occlusion-scaled ground bounce; aerial-perspective fog (sky-tinted, sun-warmed, altitude-attenuated) replacing constant horizon fog; backlit foliage translucency for canopy (matId 8); two-scale heightmap normals (0.10 m detail blended 55/45 with 0.35 m form); fbm grass albedo/blade detail; 2-octave water ripples; vibrance + split-tone grading. Both backends updated in lockstep — `--compare` 6.76/255, visual_check green, +0.6 ms/frame. Screenshots `hero_pbr/house_pbr/water_pbr/golden_pbr.png`.
- [x] **Realistic grass & foliage field (2026-08-24):** two-layer procedural flora in `shadeTerrain` (both backends, shading-level, LOD-faded 9→26 m): (1) blade-scale warped-noise micro-grass texture (fbm-warped vnoise @33/m, wind-animated via `pc.misc.y`, ~±14 % luminance + normal jitter) filling gaps; (2) structured tufts — 4–7 tapered leaning blades (0.12–0.40 m, 1.4 cm wide segments, per-blade random azimuth/lean/color, dry straw tips, dark-root/light-tip AO) for grass (matId 0/1) and irregular two-sided leaf clusters (0.5 m cells, facet normals, hue jitter) for canopy/bushes (matId 8). LOD fallback keeps far fields on the fbm `grassDetail` look — no visible seam; `--compare` 6.76/255, visual_check green, ~0 ms/frame. Screenshots `grass_closeup.png`, refreshed `hero_pbr/house_pbr/golden_pbr.png`. Known limit: blades are shading-only (terrain silhouette unchanged; geometry-level blades would need a second collision field).
- [x] **High-res grass sprite cards (2026-08-24):** replaced the blade-tuft layer for grass with **alpha-tested crossed sprite cards** ray-intersected in the raymarch: two decorrelated grids (coarse meadow tufts 0.42 m cells, 0.50–0.88 m wide; fine filler 0.17 m cells, 0.14–0.30 m), each card = 9-branch analytic `tuftAlpha` pattern (resolution-independent "texture", tapered wavy blades, wispy tips), per-card hash albedo (dark root → lit tip gradient) + two-sided up-biased normals with fbm wrinkle. Cards occlude the ground (`tc < dist`), shaded via existing PBR; gap-filling micro-grass noise + far LOD fallback unchanged. `--compare` 6.76/255, visual_check/selftest green, +0.05 ms/frame. Screenshots `grass_sprites.png` + refreshed trio.
- [x] **Grass coverage fix + blade streaks (2026-08-24):** the flora/card LOD was computed from `t - t0` where `t0` is the *world-AABB entry distance* — with the camera INSIDE the world volume `t0 < 0`, so `dist` was inflated by ~70 m and the whole grass layer silently disabled (`smoothstep(9,26)` = 0 everywhere). Fixed by computing the true camera distance `length(p - ro)` inside `shadeTerrain` (dropped the `dist` param). Near-field coverage was still patchy because short cards can't occlude steep rays: the blade pattern is now also sampled at the ground hit point (hit-space) and the fill texture switched to **anisotropic world-space vertical blade streaks** (`vnoise` stretched ~9:1 vertically, wind-sheared, 3-4 cm wide) with height-graded root/tip AO — reads as blades from every angle instead of isotropic per-pixel mush. LOD widened to 12→40 m; all green terrain (matId 0/1) is now covered within that range, far fields keep the fbm detail. Specular fireflies fixed via clamped Smith visibility (≤6.0). `--compare` 6.70/255, visual_check/selftest green, +1.6 ms/frame.
- [ ] Highly realistic foliage — leaf-cluster SDF warp (fbm-eroded clumps, twig spheres), two-sided SSS back-light + thickness-attenuated bleed + waxy specular, per-leaf albedo variation, wind vertex displacement (pc.misc.y), view-dependent thickness

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
- [x] **GaussianShader Tier 1 (real-time shading, no training):** flattened anisotropic disks
      (`shortest axis v = normal`, thin `sz=VOXEL*0.55`), shading `c=γ(c_d + s⊙Ls)` with
      `s,ρ` from `kMaterialReflection`, HDR cubemap `6×64×64` prefiltered GGX per roughness
      (`Eq.4`, `lod=ρ*6`), Fresnel Schlick, water `ρ=0.15` reflective variant, denser field
      `spacing 0.15m` + `water 0.22m` + `2.5M` budget. `splat.vert/frag` + `SplatPass` cubemap.
      Residual `c_r(SH)` kept `0` (training-needed, plumbing reserved). — `2026-08-23`
- [x] **Splat density without radius boost (gap fix):** constant `splatRadius=0.1875` (no far `grow`), gaps closed via interpolation factor `1x/2x/4x/8x` (`--splatdensity`, `I` cycle + `1`/`2`/`4`/`8` shortkeys, `rebuildSplats()`). Decimation `1/9→1/4→1/2→all` + fixed `8`-corner subgrid + `SDF|d|<0.22` filter + shuffled `earlyBudget` (`1.5M/2.5M/5M/10M`) for uniform coverage. `splat.vert` foreshorten only. Sorting optimized: `std::sort` + static-view skip + bi-frame skip for `>5M`. — `2026-08-24`
- [ ] Derive anisotropic Gaussians fitted to brick surfaces (replacing point sprites) — *partial via Tier1 disks, full covariance fit pending*
- [ ] Tile-sort + blended splat pass sharing the offscreen/blit pipeline — *currently `stable_sort` CPU, GPU radix pending*
- [x] **Minimal-raytrace shadows per `implementation_plan.md`:** one visibility ray per
      splat against SDF proxy (`scene()` heightmap+objects, 12 steps `9*d/t`, `par` via `tbb`); `shadow` `0..1` in `SplatVertexData`, `splat.frag:28` `* (1 - shadow*0.65)`, toggle `H`/`--noshadows`, rebuild via `rebuildSplats()` — `2026-08-24`
- [ ] F-key toggle becomes true representation switch; crossfade blend
- [ ] NSVF-inspired extension (M8): per-brick feature vectors decoded by tiny in-shader
      MLP → view-dependent appearance + splat-parameter generation
- [ ] **Physically-based splats (see §5):** per-splat `σ_t` + Henyey–Greenstein `g`;
      Beer–Lambert front-to-back compositing; **Weighted Blended OIT** to kill pop-in;
      **ray-marched water/fog volume** reusing SVO empty-skip; octree LOD + splat budget +
      8-bit texture quantization for scale.

#### Phase P4-T3 — GaussianShader Differentiable Training (deferred, doc-only)
**Goal:** paper-faithful `L = L_color +0.01 L_normal +0.001 L_sparse +0.001 L_reg` (`Eq.9`) training of
`c_d, s, ρ, c_r(SH deg3), Δn1/2, env 6×64×64` on top of Tier1 scaffolding.

*   **Dataset capture:** `tools/capture_dataset` (`--dump-train out/`) uses existing SVO raymarch to emit
    `images/*.png + poses.json + sunDir` from hero/house/water orbits; reuses `--shot` PPM path (`tests/visual_check.py`).
*   **Trainer:** new `tools/train_gs/` Python (PyTorch, `diff-gaussian-rasterization` or `nvdiffrast` Vulkan autodiff).
    Init from SVO splats; learn `Δn` with `Eq.5` flip, shortest-axis prior `Eq.6`, depth-normal consistency `Eq.7`
    (Sobel on rendered depth vs `n̄`), sparse `Eq.8`, env cubemap as `nn.Parameter`.
    `30k` Adam steps, `Tab.3` `0.58h` on `RTX 3090` (`vs 23h Ref-NeRF`).
*   **Export:** splat file (`pos, scale, quat, c_d, s, ρ, SH9×3, Δn`) reloadable by `SplatPass` (stride 5→8); `c_r` enables indirect reflections.
*   **Validation:** `Tab.1/2` PSNR on `NeRF Synthetic / Shiny Blender`; our `Tab.4` ablations (`w/o L_sparse`, `w/o L_normal`, `w/o c_r`, `w/o v`, `MLP vs Env`).
*   **Scope guard:** no engine training loop; viewer stays real-time `97 FPS` (`Tab.3`).
*   **Estimate:** 4–6 weeks; requires `torch` + `FetchContent` `stb_image` already present, no Vulkan validation layers needed.

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
