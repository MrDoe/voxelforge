# Plan: Fully dynamic voxelforge (VoxelsOnly branch)

**Goal.** The runtime app and both GLSL shaders derive *all* geometry — objects **and** terrain — solely from `.vxw` voxel records + `world.json`. No `scene()`/object SDF/terrain SDF in runtime C++ or in shaders. `heightmap_gen.cpp` stays as the offline asset baker (its hardcoded shapes are allowed — it only writes `.vxw` data). Splats are removed entirely.

## Core idea — a single `VoxelField`
Introduce `src/voxel/voxel_field.hpp/.cpp` built by `LayeredWorld` from merged records:
- **Terrain** (role `landscape`): per-column `terrainTop(xz)` (max record-y) + dominant `materialId`. Stored CPU-side and as the `uHeight` texture (RG32F: R=top, G=mat) for the GPU.
- **Objects** (role `object`/`scatter`): shells flood-filled to solid (existing algorithm), then a signed distance transform over each object's bbox grid (VOXEL resolution) → `objSDF[]`, `objMat[]`, `objColor[]`.
- API: `distance(p)`, `material(p)`, `color(p)`, `solid(p)`, `terrainTop(xz)`.
- **Single consumer for everything**: `fillBrick` (bake), `volume.cpp` (dense), and all probes/picking/mcp/chat_ui/tests.

This deletes every `scene()`/object call from the runtime because `VoxelField` is built *only* from records.

## Phase 1 — `VoxelField` + bake (no `scene()`)
- New `voxel_field.hpp/.cpp`: build from `LayeredWorld`'s per-layer records; expose queries above.
- `layered_world.cpp` `fillBrick` (~178): replace `scene(worldOf(...))` with `field.distance/material/color`. Terrain SDF comes from `terrainTop`; objects from `objSDF`.
- Remove the rejected mask machinery (`OB_*`, `treeBit`, `enabledMask`, `BuildCtx.enabledMask`) — superseded. Keep `m_pools`/`ChunkPool` incremental rebuild, now keyed on layer mtime → reload that layer's records → rebuild its grid → re-bake overlapping chunks.
- `load()`: build `VoxelField`; emit the `uHeight` RG32F (from `landscape.vxw`, **not** the PNG).

## Phase 2 — Shaders: delete analytic geometry
In **both** `svo_raymarch.comp` and `raymarch.comp`, remove: `houseDistance`, `treeDistance`, `treesDistance`, `rocksDistance`, `bushesDistance`, `objectDistance`, `getMaterialId` (analytic), and the GLSL `kTreeSpots`/`kRockSpots`/`kHousePos`/`kRockRadii`/`kBushCell` constant arrays.
- `map()` / `sdfAt()`: keep SVO / dense-volume SDF for **objects**, plus `min(sd, p.y - heightAt(p.xz))` for **terrain** (data-derived `uHeight`). Keep water plane (`y=-0.9`) + dome.
- `calcNormal`: terrain normal from `heightAt` gradient (kept — it's a heightfield query, not analytic shapes); object normal from `map()` gradient.
- `getMaterialId` → new: terrain ⇒ `uHeight.g` material (with height/slope/water shading inputs, all data-derived); object ⇒ **brick material** read from the SVO node (already stored in `word1.mat`). For dense, store materialId in the volume texture's spare channel.
- `softShadow`/`sdfAO`: use `map()` only (already gated). Preserve `--compare` parity (water `y=-0.9`, fog `0.0012`, ACES, bidirectional `waterHit`, `gUnderwater`).

## Phase 3 — Remove splats entirely
- Delete `src/render/splat_pass.cpp/.hpp`, `shaders/splat.vert`, `shaders/splat.frag`.
- `CMakeLists.txt:131` (drop `splat_pass.cpp`), `:145-146` (drop splat shaders).
- `main.cpp`: remove `RenderMode::GaussianSplats`, `m_splatPass`, `buildSplatData`, `rebuildSplats`, `m_layeredSplats`, all splat `Args` (`--mode splat`, `--splatscale`, `--splatdensity`, `--shadows`, `--ao`), `F`/`+/-`/splat-scale/`H`/`I` key handling; `uHeight` now sourced from `VoxelField` not `sharedHeightmap()`.
- `chat_ui.cpp:226` splat text, `mcp_server.cpp:8` splat mention — drop.
- `AGENTS.md`: remove splat run/keys; keep SVO-only scope.

## Phase 4 — Rework remaining `scene()` consumers
- `src/voxel/world.cpp` (`:53,:70`), `src/voxel/volume.cpp` (`:20` — build dense SDF from `VoxelField`), `src/voxel/picking.cpp` (`:46,:59` — use `field.distance`/`material` + central-diff normal), `src/ai/mcp_server.cpp:199` (`scene(...)`→field query), `src/app/chat_ui.cpp:157` (probe→field).
- `tests/test_authoring.cpp`, `test_world.cpp`, `test_volume.cpp`: replace `scene()`/`houseAt`/`fenceAt`/`alpacaAt` assertions with `VoxelField` queries (load world, assert `field.solid` + `material` at `kHousePos`/`kPaddockMin`/`kAlpacaSpot` — constants remain in `common.hpp` for the baker + tests).
- Remove `aiEditsRegister`/`aiEditsAt` (only fed splats/probes; `ai_edits.vxw` is already a layer). `heightmap_gen.cpp` stays unchanged (baker).

## Phase 5 — Build & verify
- `ninja -C build voxelforge` (recompiles shaders) and `ninja -C build world` if assets changed.
- `ctest --test-dir build` (unit + visual_check).
- `./build/voxelforge --compare --width 640 --height 360` (tight `meanDiff<14`, `|covSvo-covDense|<0.08`).
- `./build/voxelforge --probe X Y Z`, `--shot out.ppm --cam ...`, `python3 tests/visual_check.py` (hero/house/water coverage).

## Risks / notes
- **Terrain normals**: rely on `heightAt` (data-derived) — keeps crisp normals; baked brick SDF is coarse, so terrain SDF in `map()` comes from `uHeight`, objects from SVO.
- **Object SDF accuracy**: per-bbox EDT on flood-filled solids; ~6 objects × ~100³ grids ≈ tens of MB — fine.
- **Discards** committed mask work (`fd36a64`) per user's rejection.
- `uHeight` source changes from PNG→`landscape.vxw`; `heightmap.png` no longer required at runtime (baker still writes it).
