---
title: Arbitrary-large landscape rendering
tags: [architecture, streaming, world-size, lod, design]
sourceRefs: [src/voxel/common.hpp, src/voxel/layered_world.hpp, src/voxel/layered_world.cpp, src/voxel/chunk_store.hpp, src/voxel/voxel_field.hpp, src/render/svo_pass.cpp, docs/architecture.md, AGENTS.md]
lastReviewed: 2026-11-19
---

# Arbitrary-large landscape rendering

The engine today is a **fixed 102.4 m cube**. `WORLD=102.4`, `VOXEL=0.1`,
`CHUNK_N=64`, `GRID_N=16` (102.4 / 6.4 = 16 chunks per axis) are `constexpr`
in `src/voxel/common.hpp` and baked into the lattice math, the chunk index,
the GPU SSBO sizing, the height texture, the shadow volume, the water plane,
and the baker. Everything is loaded at startup: `LayeredWorld::load()` merges
every enabled layer, builds one global `VoxelField`, synthesises all
`GRID_N³ = 4096` chunk octrees, and uploads the whole `GpuWorld` in one shot.

To render *arbitrary* (kilometre-scale or unbounded) landscapes we have to
break four couplings: **fixed world size**, **whole-world residency**,
**monolithic GPU buffers**, and **float32 world coordinates**. None of them is
a single edit; each is a milestone. The good news is that the engine already
has most of the machinery a streaming world needs — it just applies it to a
fixed box.

## What already helps

- **Per-chunk SVO handles are chunk-local** (`GpuWorld::chunkInfo`,
  `common_svo.glsl` `chunkBases`): `SvoPass::patchChunk()` re-uploads one
  chunk's pool without touching the rest. This is the exact primitive a
  streaming resident set needs.
- **Paged surfel buffer** (`SplatPass::m_chunkStart/Count/Cap`): untouched
  chunks stay put; a chunk that outgrows its slot relocates. Same shape as a
  streaming upload.
- **Async rebuild thread** (`LayeredWorld::kick`): a rebuild runs off the
  render thread and is swapped in via `consumeRebuild()`. Loading a chunk
  region is the same pattern.
- **Resident `ChunkPool`s** kept across reloads, plus dirty tracking by layer
  AABB. Only dirty chunks rebuild.
- **Micro objects are already separate SVO worlds** with their own transforms
  and overlay pass — a precedent for "worlds that are not the world".
- **LOD rings** for surfels (`VF_LOD1`/`VF_LOD2` at 20/60 m, material-split
  blocks) and the `VF_LOD` toggle. The *mechanism* exists; it is just not
  applied at chunk granularity.
- **`ChunkStore`** is region-limited: `rebuildDirty` re-derives only the edit
  AABB ± 12 cells. The same locality is what makes per-chunk field builds
  tractable.

## The four couplings to break

### 1. Fixed world size (`WORLD`, `GRID_N`, `kLatN`, `kObjVolN`)

These constants appear in:
- `common.hpp`: `WORLD`, `VOXEL`, `CHUNK_N`, `GRID_N`, and derived
  `kLatN = WORLD/VOXEL`, `kChunkCount = GRID_N³`.
- `voxel_field.cpp`: `colTop`/`colMat` arrays sized `kLatN²`; object volume
  `kObjVolN³` (256³).
- `chunk_store.cpp`: `m_chunks.resize(kChunkCount)`; `sampleWorld` does
  `int((p.x + 0.5*WORLD)/VOXEL)`.
- `heightmap.hpp`: 2048² texture covering `[-8,24]` m, sampled with
  `wx/WORLD + 0.5`.
- Shaders: `common_svo.glsl` uses `uPushB.x = WORLD`, `b.x = GRID_N`,
  `objDist` clamps `p/WORLD + 0.5`.
- `layered_world.cpp`: chunk index `cx + cy*GRID_N + cz*GRID_N*GRID_N`,
  `chunkRangeFromAABB` clamps to `GRID_N-1`, AABB→chunk math divides by
  `WORLD`.

**Change:** introduce a **chunk coordinate** as the primary spatial key
(`ivec3` of chunk indices, unbounded) and make `WORLD`/`GRID_N`/`kLatN`
per-region or procedural rather than global. The lattice index becomes
`worldToChunk(p)` + local cell, not a single `int((p+0.5*WORLD)/VOXEL)`.
This is the deepest change: every `+ 0.5f*WORLD` offset in the codebase
(voxel_field, picking, shaders, irradiance) must move to a
camera/region-relative frame.

### 2. Whole-world residency

`LayeredWorld::load()` reads *all* layers, merges them into one `VoxelField`,
and synthesises every chunk. `VoxelField::build()` pays ~13 s of EDT over
72k components (see `[[concepts/load-time-field-build]]`) — and that is for a
102 m world. For a 10 km world this is impossible.

**Change:** build the field **per chunk / per region tile**, on demand, in the
existing async thread. The merge step (`readLayered`) already dedupes by
lattice key with first-wins priority; make it region-scoped: only read records
whose AABB intersects the requested region. Layers become *streaming sources*
with spatial indices (R-tree or grid of chunk coords) instead of "read whole
file, then merge".

The heightmap must stop being a single 2048² image: either tile it
(virtual texturing / clipmap) or replace `heightmap_gen` with procedural
noise (`fbm2` already exists in `common.hpp`) plus authored overlays. The
`objVol` shadow volume (256³ r8) must become a per-region or clipmap
volume, or be dropped in favour of the per-surfel baked shadow the splat
path already uses.

### 3. Monolithic GPU buffers

`SvoPass::setWorld()` destroys and re-creates the five SSBOs for the whole
world. `GpuWorld::chunkGrid` is `GRID_N³` ints. For streaming, the resident
set is a *subset* of an unbounded grid, so a fixed-size array cannot
represent it.

**Change:** keep the existing chunk-local handle indirection (`chunkInfo` per
chunk) but make the resident set a **sparse map**: chunk coord → slot index
in a growable pool, with a free-list for eviction. `patchChunk`/`patchChunkSurfels`
already do per-chunk updates; add `evictChunk(coord)` that frees the slot and
marks it empty in the indirection table. The surfel path's paged buffer is
the template. Bindless descriptors or a single large buffer with offsets both
work; the existing grow-on-demand `growSsbo` is a stopgap, not a strategy.

The SVO raymarcher's `chunkGrid` (the root-handle table the shader indexes by
chunk coord) must become a hash map or a clipmap; the simplest is a
**clipmap of chunks around the camera** (fixed number of resident chunks in
a toroidal grid), which keeps the table fixed-size and only changes which
world chunks occupy each slot. This is the same trick terrain engines use for
heightfields.

### 4. Float32 world coordinates

At 10 km, `float32` has ~1 mm precision and gets worse; at 100 km it is
centimetres. The camera, picking, surfel positions, and the SVO push
constants (`camPos`, etc.) are all `glm::vec3` (float). Origin-shifting
(camera-relative rendering) is mandatory before any of the above ships.

**Change:** store world positions as `double` (or as chunk coord + local
float) in CPU data; subtract the camera's chunk/origin before uploading to
GPU push constants and surfel buffers. The shader already receives `camPos`
as a vec3; if the camera is the origin, `camPos` becomes ~0 and world
coordinates stay small. Picking (`rayPick`) and collision must use the
double-precision path.

## LOD: the thing that actually makes it fast

Streaming alone does not make a 10 km world renderable; you cannot raymarch
10 cm voxels at 10 km. The engine already has the *hooks*:
- Surfels have LOD rings (surfelize 2×2×2 / 4×4×4 merged terrain, draw-time
  selection at 20/60 m) and the `VF_LOD` toggle.
- The SVO has per-chunk octrees that subdivide down to 8³ bricks; a chunk
  that is far can stop subdividing early (a coarser octree = fewer nodes).

**Plan:** chunk-level LOD. Near chunks: full 10 cm SVO + surfels. Mid:
coarser octree (e.g. 40 cm effective) + merged surfels. Far: a heightfield
mesh or a single merged "terrain chunk" splat cloud. The existing
`classifyBox` already distinguishes empty / solid / subdivide; add a
distance term so a far non-empty chunk becomes a solid/merged terminal
instead of recursing to bricks. Micro objects already demonstrate the
pattern: a separate, coarser representation (the mesh actor path) for
things that do not need voxel fidelity.

## Phased roadmap

| phase | work | reuses |
|---|---|---|
| 0 | Decouple: make chunk grid dynamic (`std::unordered_map<ChunkCoord, …>`); remove `GRID_N` from indexing; keep `WORLD` as a *default* not a constant | `chunk_index.hpp`, `chunk_store` |
| 1 | Floating origin: double-precision CPU coords, camera-relative GPU upload, picking/collision on double path | `picking.cpp`, `collision` |
| 2 | Region-scoped merge: spatial index over layer records; load only intersecting records per region | `readLayered`, `LayeredWorld::parseAndComputeDirty` |
| 3 | Per-region field build: move `VoxelField::build` to a per-tile async job; reuse the existing hash cache | `voxel_field.cpp`, `kick` thread |
| 4 | Chunk streaming manager: resident set around camera, LRU eviction, async load/unload, clipmap indirection table | `patchChunk`, `patchChunkSurfels`, `evictChunk` (new) |
| 5 | Terrain tiles: split heightmap into a quadtree/virtual texture, or procedural noise + authored overlays | `heightmap.hpp`, `heightmap_gen` |
| 6 | Chunk LOD: distance-based subdivision cutoff; far chunks as merged surfels or heightfield mesh | `classifyBox`, surfel LOD rings |
| 7 | Live edits at scale: region-addressed overlay (already per-chunk flags; just stream the file) | `OverlayWriter`, `runtime_edits.vxw` |

## What must NOT be done

- **Do not just raise `WORLD`/`GRID_N`.** Memory is `O(n³)` in chunks and
  `O(n²)` in terrain; a 10× world is 1000× the chunks. The current 4096
  chunks already produce ~17.6 s of load time.
- **Do not skip the floating origin.** Shipping large worlds on float32
  produces visible jitter at a few kilometres and is unrecoverable later.
- **Do not rebuild the whole field per region change.** The existing
  `m_prevHashes`/`m_prevOuts` cache is the model; extend it, do not
  replace it.
- **Do not break test determinism.** Headless `--shot`/`--selftest` rely on a
  fixed, fully-resident world. Streaming must expose a "pin resident set"
  mode (e.g. `VF_NO_STREAM=1` or a fixed camera seed) so gates stay
  byte-reproducible. The `VF_TEST_BEHAVIOUR_FREEZE` precedent shows how to
  pin a moving system for measurement.

## Open questions

- Clipmap (fixed resident grid, toroidal) vs. sparse hash (unbounded, variable
  residency)? Clipmap is simpler and matches the existing fixed-size GPU
  tables; sparse hash is more flexible. Recommend clipmap first.
- Should terrain become procedural (infinite) or stay authored-but-tiled?
  Authored tiles preserve the current art pipeline; procedural needs a new
  baker. Likely a hybrid: procedural base + authored overlay layers (the
  existing `ai_edits`/object-layer model already is "authored overlays").
- LOD transitions: pop-in vs. morphing. The surfel LOD already switches at
  fixed distances; chunk LOD needs the same hysteresis to avoid flicker.
- Collision over streamed chunks: the fly camera's 27-sample sphere sweep
  (`collision.md`) must query the resident store; unloaded chunks are
  treated as "unknown/solid" or the player is clamped to the loaded region.

## Cross-references

- `[[concepts/load-time-field-build]]` — the 13 s EDT cost that motivates
  per-region builds.
- `[[concepts/micro-voxel-svo-overlay]]` — precedent for separate, transformable
  SVO worlds.
- `[[concepts/layer-placement]]` — owner IDs and transforms that must survive
  streaming.
- `[[concepts/collision]]` — the solidity oracle that must become
  streaming-aware.
- `[[concepts/sceneries]]` — the per-scenery folder model; scenery switching is
  now in-session (World panel SCENERY group, flushes leaving scene's edits,
  writes the pointer, full rollback on failure; headless hook
  `VF_TEST_SCENERY_SWITCH="a,b"`). **Never hardcode scenery items** in C++/GLSL
  (material ids, layer names, positions, card names) — they must be data in
  `world.json`/shared manifests resolved at load time; this is the prerequisite
  that makes a streamed world composable. A "large world" is likely a bigger
  scenery with more tiles, or a sequence of sceneries streamed/swapped, not a
  new concept.
- `[[concepts/improvement-roadmap]]` — the existing roadmap; this page is the
  "scale" axis, orthogonal to its "quality/perf" axis.
---

*Filed from a design query (2026-11-19). No code changed.*
