# Voxelforge — Thread Summary

**Date:** 2026-08-23  
**Branch:** `master`  
**Last commits:** `b534d7e` (denser splat subgrid + unified voxel file + on-demand terrain) → `54b3f1b` (German QWERTZ +/-)  
**Mode at end:** `build` (started `plan`)

---

## 1. Objective — Evolution

1. **Initial:** Get the chunked Sparse-Voxel-Octree (SVO) renderer to photorealistically render the river-valley landscape and reach parity with the dense reference backend.
2. **Mid-thread:** Integrate the external Copilot share *“Physically-Based Billboard Splats in a Fixed 3D Grid”* (§5) and the minimal-raytrace splat guide (`implementation_plan.md`) into `ImplementationPlan.md`.
3. **Later (user-driven):**
   - Add controllable sun (`--sun`) and fix frozen water ripples.
   - Black voxels / cut-open objects → heightfield terrain.
   - Replace procedural hill+river with a real `16-bit 2048² 5 cm/texel` heightmap (`assets/heightmap.png`) and store the **entire world in one binary voxel file** (`assets/world.vxw`, `VXW v1`, per-voxel `u16³ pos + RGBA + reflectivity/roughness/material`, `2 words/voxel` bricks). Terrain is now **once on-demand** (`ninja world` / `heightmap_gen`), the file is the editable template.
   - “No more hut/pines/boulders” → then “add *real* house + tree at the river, realistic scale, layered bottom-up”.
   - Splats are **alternative rendering only**, much denser than voxels, with interpolated subgrid; landscape + house + all objects must live in the one voxel file at unified scale.
   - Constant-mist washout, waves when moving, dense flicker, tree half-clipping at distance.
   - Vivid palette request.
   - Foliage → ground bushes (`bushesAt()` 6 m grid, `35 %` density, grass-only).
   - Splats front should occlude far; splat scene must always be derived from voxel scene.

---

## 2. Important Technical Details

**World constants (still):** `WORLD=102.4f`, `VOXEL=0.1f`, `CHUNK_M=6.4f` (`CHUNK_N=64`), `GRID_N=16`, `BRICK_N=8`, `BRICK_VOXELS=512`, `BRICK_WORDS=1024` (since `2 words/voxel`), `WATER_LEVEL=-0.9`.

**Push constants — `RaymarchPush` is now 128 B (Vulkan minimum):**
```cpp
struct alignas(16) RaymarchPush {
  vec4 camPos, camRight, camUp, camFwd;
  vec4 a; // tanHalfFov, aspect, extentX, extentY
  vec4 b; // worldSize, maxEncodedDist/voxelSize, frameIdx (legacy ripple used this)
  vec4 sunDir; // normalized toward sun
  vec4 misc;   // x=splatScale, y=animTime(s), z=shadingMode(0 BBSplats/1 3DGS), w=aoEnabled
};
```
`a = (tanHalfFov, aspect, extentX, extentY)`; `b` semantics differ per backend (SVO `b=(WORLD,VOXEL,GRID_N,0)`).

**Heightmap encode:** `u16 = clamp((h - (-8))/32)*65535`, `m = -8 + u/65535*32`, `WATER_LEVEL=-0.9`, `kPadY=-0.40` at `kHousePos=(6.5,12.5)`, `kTreePos` removed in favor of `kTreeSpots[6]` + `kRockSpots[3]` + procedural bushes.

**Brick packing:** `word0 = r|g<<8|b<<16|sdfByte<<24` (`sdfByte = int8(sdf/VOXEL)`), `word1 = a(255)|refl<<8|rough<<16|matId<<24` (`kMaterialReflection[9]`).

**Shaders must stay in parity** (`svo_raymarch.comp` ↔ `raymarch.comp`) for `--compare` to pass; every shading tweak goes in both.

**Compare thresholds tightened:** `meanDiff <14/255` and `|covSvo-covDense|<0.08` (was `32`/`0.15` which hid the hollow-voxel bug).

---

## 3. Work State

### Completed

* **SVO sphere stub** → real `map()` traversal (chunk grid → octree → `8³` bricks, mirroring `World::sample`) + `brickVoxelSdf` fix (`raw*VOXEL` not `raw/127*VOXEL`).
* **Dense reference** set to `vol.worldSize = WORLD` (256³ `@0.4 m` over full span) for fair parity.
* **Photorealistic shading parity**: ACES (`col*1.25` → later `1.0`), analytic `y=-0.9` water plane with ripple normals, Fresnel, planar-reflection march, sun glint, underwater tint, hemispheric sky + ground bounce ported to both backends.
* **Water-reflection parity** via shared `shadeTerrain()`/`brickAlbedo()`.
* **Copilot plan woven** into `ImplementationPlan.md §5` (material `σ_t`/HG `g`, Beer–Lambert, Weighted Blended OIT, octree LOD, etc.) and renumbered Commands to §6.
* **`--sun <elev> <azim>`** (default `34°/238°` golden-hour, now hero-shot-friendly) threads `sunDir` through `RaymarchPush` (112→128 B), `kSunDir` mutable global set from push in both shaders. Verified with low/high sun mean diff `10.5/255`.
* **Frozen ripples** fixed: `pc.a.w` (extentY constant) → `pc.b.w` (frameIdx) → now `pc.misc.y` (real seconds `m_animTime += dt`) so interactive ripples animate and `--animtime` is deterministic for headless.
* **Heightfield terrain (`M8`)**: `tools/heightmap_gen.cpp` writes `assets/heightmap.png` with a 80-line stored-deflate 16-bit PNG writer (bug `rowBytes=(1+w)*2` → `1+w*2` fixed, PIL-validated) and now also builds the full `World` and serializes `assets/world.vxw`. Terrain: ridged `fbm` hills (`amp 7.5*bankRamp+4.5*farRamp`) + meandering `riverZ/w` + level-bed bowl, water coverage `3.5%`.
* **Object removal → re-addition**: hut/pines/boulders deleted, then **layered riverside house + tree** rebuilt bottom-up (foundation `4` → 8 alternating log courses `6` with door/windows carved via `sdBoxF`+`max(-door)` → 10 stepped shingle layers `7` → chimney `4`; tree: root flare `0.37`→`0.165` tapered trunk `6` + 4 foliage tiers `8` `~7.5 m`). Realistic vs river (`full width ~6.5 m`, house `5.2×4.5 m`, tree `~7.5 m`). Ground pads flattened in generator. Palette extended `9` entries, `kMaterialReflection[9]`.
* **Mist fix**: fog density `0.006→0.0012` both backends; luminance spread `23→39` (`p5 214→196`) at canonical view.
* **SVO tunneling/hollow-voxel fix**: empty chunk/octant now returns `max(-sdBox(p,cmin,cmax), VOXEL*0.5)` (conservative `sdBox` distance to the empty cell's AABB) instead of `6.0`; march budget `320→900` and 6-step bisection on `s<eps` (`eps=VOXEL*0.45`, `lo=max(t-2,0)`). Black-in-object dropped `41%→0.0%`.
* **Heightfield shadows & fog parity**: analytic `softShadow` marches `min(sp.y-heightAt, objectDistance)` identically, `calcNormal` branches to exact primitive normals for objects else `heightAt` heightfield normal (`e=0.35`); `softShadow` now includes objects — house/trees cast shadows (`sun-flip 28.9/255`).
* **Shoreline foam + grading + sun visibility**: `vnoise` streaks masked to thin `shoreF=1-smoothstep(0.02,0.26,WL-bedH)` (`vnoise*3.7` drift via `misc.y`), `0.50` blend; post-ACES saturation `1.14` and sun glow `0.60/0.52`; golden-hour defaults and hero-shot spawn `(-16,6.5,-14)→(6.5,0.8,11)` put the cabin mid-frame with raking light.
* **Valley scatter**: `kTreeSpots[6]` + `kRockSpots[3]` (now 6+3) ground-hugging via `heightAt` sampled bases, added to `common.hpp` `treesAt()`/`rocksAt()` and mirrored as identical GLSL (`svo/raymarch.comp:232`).
* **Palette vividness pass**: second vivid pass (`grass 0.07,0.52`/`0.16,0.68`, `rock 0.48`, `wood 0.62,0.33`, `foliage 0.04,0.52`) + grading `1.14→1.55`, exposure `kSunCol 2.7→1.35` + disc `0.60`/glow `0.52`, ripple `0.06→0.04` & freq `9→4.5`, horizon distance prefilter reverted (was causing terrain swimming).
* **VXW v1 world file** (`src/voxel/worldfile.{hpp,cpp}`): `header 64 B` (`VXWF`/`1`, `worldSize/voxelSize/waterLevel/gridN/brickN`, CRC32) → SVO buffers → `16 B` records (`u16³ pos + RGBA + refl/rough/mat`). `bricks` now `2 words/voxel`. App loads it directly (`SVO world loaded from … 262 MB`) and falls back to procedural; splats derive from the scene SDF (`scene()`), just another rasterization. Generation is **once on-demand** (`heightmap_gen` is no longer a build dependency; `ninja world` or `heightmap_gen assets/heightmap.png assets/world.vxw`).
* **Splats as alternative only + denser grid**: lattice `192³→` `spacing 0.31→0.21 m` in splat mode (`SVO` `~1.47 M` splats) plus 8-neighbour interpolated subgrid at `spacing*0.25` where `|scene|<0.22`; radius `spacing*1.25`; point-size cap `48→512 px` (the real cause of near-field gaps) and near-field growth `1+0.8*(1-smoothstep(4,30,dCam))`. Colors baked via `scene()` `kPalette` and heightmap gradient normals; water grid added. Verified `mid-range gaps 2.0%→0.8%`, luminance parity `102 vs 227 → 226 vs 227` after plateau alpha `smoothstep(0.55,1)`.
* **Dense shimmer fix**: `rhi/resources.cpp` builds a 9-level 3-D mip chain (CPU box-filter, `int8` min-abs for `SDF` to preserve zero-crossing) and `sampler maxLod 12` `aniso 4×`; `raymarch.comp:47` now `textureLod(..., lod)` with `lod=log2(max(dist*0.015,1))` for `SDF`/`albedo` at far distances.
* **Splat front-covers-far**: `splat_pass` now keeps `m_origPos/Col` and `updateSorting(camPos,camFwd)` sorts `stable_sort` back-to-front (`dot>`) with `0.005` tie-breaker before each `record` (headless + interactive). Front correctly occludes far.
* **Splat house/water parity**: SVO splats were terrain-height-band only, so house was missing (`wood 0`). Changed to `abs(scene(p).d)<0.22` and added water grid for dense as well; dense now supplements its coarse `0.145` volume splats (`0.38` unified to `0.38`) with analytic house lattice for thin logs.
* **German QWERTZ `+`/`-`** fixed: `src/app/main.cpp:868` now checks `EQUAL/KP_ADD/RIGHT_BRACKET/APOSTROPHE` for `+` and `MINUS/KP_SUBTRACT/SLASH` for `-` (`exp2(1.6*dt)` hold).
* **House scale unified**: all splats now `radius = spacing*1.25` (`denseIsPrimary` no longer `0.145` vs `0.56`); `world.vxw` unified scale for landscape + all objects and on-demand terrain noted in `ImplementationPlan.md` and `Muse` instructions.
* **Build & tests green** throughout: `16/16` unit tests (near-surface agreement `123/123`), `compare` ~`4-7/255`, `visual_check`, `smoke 400` `0.80 ms`.

### Active
* Polish of the heightfield shadow + PBR path (current `shadeTerrain` still screen-space penumbra `9*s/t`, `1.14` grading — next P1 items are proper Hosek-Wilkie sky, half-res GI + variance filter, and TAA with reprojection).
* `splat.frag` still premultiplied `ONE/ONE_MINUS_SRC_ALPHA` painter blending — correct with sorting but heavy; next P4 will move to Weighted Blended OIT per §5.4.

### Blocked
* None. LunarG validation layers not installed on this machine.

## 4. Next Move

1. Finish the `P1` realism trio still unchecked in `ImplementationPlan.md:179`:
   - Hosek-Wilkie/Preetham sky replacing the gradient `skyColor`.
   - `1-bounce diffuse GI` (half-res temporal + variance-guided filter).
   - `TAA` with `depth/normal` history reprojection (the current `pc.misc.y` time and `RaymarchPush` layout already reserve `misc`).
2. Keep `PBR-ish` `kMatRefl` wiring tight — currently `shadeTerrain` reads `kMatRefl` via `getMaterialId(p)` analytic; next step is to read the second brick word's `refl/rough` directly in `brickAlbedo` for SVO so the file drives shading.
3. Add the `tiling`/`foam` polish and the instanced `pines/rocks/grass tufts` only after `TAA/GI` land, to keep `visual_check` stable.

## 5. Relevant Files

* **Heightmap & world asset**: `tools/heightmap_gen.cpp` (terrain `terrainHeightAt`, pad, stored-deflate PNG, `World::build()`), `src/voxel/heightmap.{hpp,cpp}` (`2048²`, `5 cm/texel`, `sharedHeightmap()` lazy, `sample`/`gradient`), `src/voxel/worldfile.{hpp,cpp}` (`VXW v1` 64 B header + CRC32, `2 words/voxel` bricks).
* **Scene truth**: `src/voxel/common.hpp` (`WORLD=102.4`, `VOXEL=0.1`, `GRID_N=16`, `kPalette[9]`, `kMaterialReflection[9]`, `kHousePos/KPadY/kTreeSpots/kRockSpots/kBushCell`, `materialAt()`, `houseAt()`/`treeAt()`/`bushesAt()`/`rocksAt()`/`scene()`).
* **SVO world build**: `src/voxel/world.cpp` (`kMaxDepth=3`, `classifyMargin`, `BRICK_WORDS=1024`, `bOff` fix).
* **GPU passes**: `src/render/raymarch_pass.{hpp,cpp}` (`RaymarchPush` `128 B`, `mipLevels` 9, `textureLod`), `src/render/svo_pass.{hpp,cpp}` (`binding 6` `uHeight r32f`), `src/render/splat_pass.{hpp,cpp}` (`posRadius`/`albedoAO`/`normalMat` 3×`vec4`, `updateSorting()` `stable_sort`).
* **Shaders** (must stay in parity): `shaders/svo_raymarch.comp` (`heightAt()` bilinear `uHeight`, `sdBox()`, `brickVoxelSdf`/`brickSample` stride `2`, `map()` `sdBox` empty handling + 6-step bisection, `softShadow` `min(heightAt,objectDistance)`, `calcNormal` heightfield vs object branch, `waterHit` `underWater()`), `shaders/raymarch.comp` (same helpers, `sdfAt()` `textureLod`, `albedoAt()`), `shaders/splat.vert` (`posRadius/albedoAO/normalMat`, `grow`, `misc.x` scale, cap `512`), `shaders/splat.frag` (`vnoise`, `hashN`, `getMaterialId()`).
* **App**: `src/app/main.cpp` (`Args` `--sun`/`--splatscale`/`--animtime`/`--probe`, `m_sunDir`/`m_splatScale`/`m_animTime`/`m_animTime dt`, `view 2.0` house/tree distance, fog `*0.002`, grading `1.55`).
* **Tests & screenshots**: `tests/test_world.cpp` (riverbed probe `0,-3.6,5`), `tests/test_worldfile.cpp` (`worldfile` roundtrip/corruption), `tests/visual_check.py` (stdlib PPM, `black-in-silhouette <5%`), `tests/screenshots/` (`house.png`, `hero.png`, `valley.png` etc.).
* **Build**: `CMakeLists.txt` (`vf_core` `worldfile.cpp`, `heightmap_gen` `vf_core`, `vf_heightmap` custom target, `vf_render` `taa` pending, `VOXELFORGE_ASSET_DIR`, `VOXELFORGE_SHADER_DIR`).
* **Docs**: `ImplementationPlan.md` (§5 woven Copilot splats, P4/P1/P2 backlog, §6 Commands), `implementation_plan.md` (M7 minimal-raytrace guide).

## 6. Commands

```bash
python -c "from PIL import Image; Image.open('in.ppm').convert('RGB').save('out.png')"
ninja -C build world                    # on-demand heightmap+world (rose from ~6 s)
ninja -C build && ./build/heightmap_gen assets/heightmap.png assets/world.vxw
./build/voxelforge                      # SVO landscape (default)
./build/voxelforge --mode splat         # splat preview (F to toggle, B/O shading/AO, +/- scale)
./build/voxelforge --sun 34 238         # golden-hour long shadows
./build/voxelforge --selftest --cam X Y Z TX TY TZ --width 640 --height 360
./build/voxelforge --compare            # tightened to <14/255 + <8% coverage delta
./build/voxelforge --shot out.ppm --cam X Y Z TX TY TZ --width 960 --height 540
./build/voxelforge --probe X Y Z
./build/voxelforge --animtime 1.2 --shot out.ppm
ctest --test-dir build                  # unit_tests + visual_check
python3 tests/visual_check.py build/voxelforge
```
