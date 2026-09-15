# Architecture

## The one invariant

**Geometry is data.** Every surface in the world — terrain, cabin, trees, AI
builds — is derived from `.vxw` voxel records at runtime. There is no analytic
scene code in the renderer or the shader, no merged cache file, no second
backend. Splats, the dense reference raymarcher and analytic runtime geometry
were removed on the `VoxelsOnly` branch (see
[`history/rework.md`](history/rework.md)). Anything new arrives as records.

Consequences worth internalizing:

- `VoxelField` (`src/voxel/voxel_field.{hpp,cpp}`) is **the load-time geometry
  oracle**; picking, probes, SVO synthesis and tests all consume it. The
  runtime-explicit `ChunkStore` (`src/voxel/chunk_store.{hpp,cpp}`, M0) is
  adopted lazily from the synthesized pools and becomes the mutation path for
  live editing (see [Live editing](#live-editing-m0-chunkstore)).
- `shaders/svo_raymarch.comp` is the only render shader; it reads baked data
  textures/buffers only.
- New content = new `.vxw` records (baked by `tools/heightmap_gen.cpp` from
  baker-side analytics in `src/voxel/common.hpp`, written by MCP/chat via
  `EditableWorld`, or hand-authored data).

## End-to-end data flow

```
                    bake time (explicit)                      runtime (continuous)
┌─────────────────────────────────────────┐   ┌──────────────────────────────────────────────────┐
│ tools/heightmap_gen.cpp                 │   │ src/ai/*  LLM / vf_mcp tool calls                │
│  terrainHeightAt() + common.hpp shapes  │   │        │ writes                                  │
│  → assets/heightmap.png                 │   │        ▼                                         │
│  → landscape.vxw + object layers .vxw   │   │  assets/ai_edits.vxw  (+ world.json toggles)     │
│  → world.json (ai_edits preserved)      │   │        │ mtime/size poll every ~0.5 s            │
└─────────────────────────────────────────┘   │        ▼                                         │
                                              │ LayeredWorld::load()                             │
                                              │  merge enabled layers, first-wins per cell       │
                                              │  landscape columns → colTop/colMat               │
                                              │  object cells → connected components             │
                                              │        ▼                                         │
                                              │ VoxelField::build()                              │
                                              │  flood-fill objects to solids + Dijkstra EDT     │
                                              │  height texture, presence grid, shadow volume    │
                                              │        ▼                                         │
                                              │ synthesize(): per-chunk octrees (threaded)       │
                                              │  resident ChunkPools → incremental rebuilds      │
                                              │        ▼                                         │
                                              │ GpuWorld ──► SvoPass SSBOs                       │
                                              │ heightTex/objVol ──► images                      │
                                              │        ▼                                         │
                                              │ svo_raymarch.comp → TAA → swapchain              │
                                              └──────────────────────────────────────────────────┘
```

## Module tour

### `src/voxel/` — world core

| file | responsibility |
|---|---|
| `common.hpp` | Load-bearing constants (`WORLD=102.4`, `VOXEL=0.1`, `CHUNK_N=64`, `GRID_N=16`, `WATER_LEVEL=-0.9`), `kPalette[9]`/`kMaterialReflection[9]`, deterministic noise (`hash2`/`fbm2`), terrain material banding, SDF primitives and **baker-side analytic shapes** (`houseAt/treesAt/rocksAt/bushesAt/fenceAt/alpacaAt`, stamps). NOT linked into any renderer path — consumed by `heightmap_gen` sweeps and tests as authoring truth. |
| `worldfile.{hpp,cpp}` | VXW v1 container I/O with CRC32 validation; JSON manifest load/write; `readLayered()` priority merge. See [world format](world-format.md). |
| `layered_world.{hpp,cpp}` | Runtime world owner: manifest load/poll, priority merge into column/object arrays, per-chunk SVO synthesis with resident pools, stats, dirty tracking. See below. |
| `voxel_field.{hpp,cpp}` | Load-time geometry oracle built from merged records: per-column terrain tops, object components flood-filled to solids with two-pass Dijkstra signed-distance grids stored in a sparse open-addressing hash; emits GPU textures. |
| `chunk_store.{hpp,cpp}` | Runtime-explicit sparse voxel store (M0): per-chunk `Empty/Solid/Explicit` states, 8³ bricks + solid boxes, cell edits, local SDF-band + octree rebuild. Lazily adopted from the resident pools by `LayeredWorld::store()`. |
| `chunk_index.hpp` | Canonical z-major chunk indexing shared by SVO, surfel and store paths. |
| `editable_world.{hpp,cpp}` | Persistent `ai_edits.vxw` writer: box/cylinder/ellipsoid/stamp rasterizers, layer import, manifest bookkeeping. See [AI editing](ai-editing.md). |
| `heightmap.{hpp,cpp}` | 2048² 16-bit PNG loader + bilinear sample/gradient. Baker/test-side only since the rework. |
| `picking.{hpp,cpp}` | CPU `rayPick()` against `VoxelField`, screen→ray helper, lattice↔world conversions. |

### `src/render/` + `src/rhi/`

| file | responsibility |
|---|---|
| `render/svo_pass.{hpp,cpp}` | Compute pipeline around `svo_raymarch.comp`; owns the five SSBOs, descriptor set, selection/hover UBO; defines `RaymarchPush` (128 B). |
| `render/taa_pass.{hpp,cpp}` | TAA resolve compute pass (AABB-clamped history blend, no reprojection). Interactive mode only. |
| `rhi/context.*` | Vulkan instance/device/VMA allocator, queue, `immediateSubmit` helper. |
| `rhi/resources.*` | `Buffer`/`Image3D` factories, staging uploads, 2D readback, `transitionImage` (sync2). Note: some `VkPipelineStageFlags2` values exceed 32 bits — cast carefully. |
| `rhi/swapchain.*` | Swapchain with `PresentPolicy {PreferMailbox, Immediate}`, recreate-on-resize. |

### `src/app/` — the program

- `main.cpp` — window, frame loop, camera/input, HUD ("World layers" panel),
  `Ctrl+LMB` picking, headless modes (`--selftest/--smoke/--shot/--probe`),
  swapchain blit + ImGui dynamic-rendering wrap, env-var test hooks.
- `chat_ui.{cpp,hpp}` — ImGui chat window; worker thread for LLM requests,
  tool dispatch into `EditableWorld`, immediate reload trigger, unknown-tool
  self-correction retry.

### `src/ai/` — LLM integration

- `ollama_client.{hpp,cpp}` — dependency-free POSIX-socket HTTP client for
  Ollama **and** OpenAI-compatible servers; sticky chat-path auto-detection.
- `tools.hpp` — fuzzy tool-name normalization + argument defaults
  (header-only on purpose: unit-testable without ImGui/networking).
- `system_prompt.hpp` — the editor persona: world constants, anchor rule,
  palette ids, the six exact tool names.
- `mcp_server.cpp` — `vf_mcp` stdio JSON-RPC MCP server (see
  [AI editing](ai-editing.md#vf_mcp-protocol)).

### `tools/`

- `heightmap_gen.cpp` — offline baker: ridged-fbm river valley, hand-rolled
  stored-deflate PNG writer, sweeps analytic shapes into per-column record
  layers, writes `world.json` while preserving an existing `ai_edits.vxw`.
- `scene_slice.cpp` — `vf_slice`: ASCII cross-sections of the live field
  (includes AI edits). Primary authoring feedback loop.

## Live editing (M0 store + M1 splat patch)

The target runtime model is **one virtual voxel space**: terrain and objects
are the same data (cells with packed appearance, response and material), and
terrain-vs-object is only metadata plus derived acceleration. The first two
milestones landed as an explicitly stored, chunk-keyed world with a live GPU
patch path:

- `src/voxel/chunk_store.*` owns per-chunk voxel data in canonical z-major
  chunk order (`src/voxel/chunk_index.hpp`). Chunk states are `Empty`,
  `Solid` (uniform underground fill, material resolved from the base column
  grid when materialised) and `Explicit` (sparse 8³ bricks + solid boxes in
  the exact GPU brick layout).
- `LayeredWorld::store()` adopts the resident `ChunkPool`s lazily: the pool
  octree is walked once, lifting bricks and solid sub-boxes into canonical
  data. `VoxelField` remains the bootstrap oracle; the store is the runtime
  edit path.
- `ChunkStore::apply()` takes cell-level `Set | Clear | Paint` edits; dirty
  chunks rebuild their local signed-distance band (two-pass 3-4-5 chamfer
  transform seeded from sign boundaries + neighbour-chunk face SDFs) and
  regenerate the octree pool from the sparse cells. Edits never touch the
  base field.
- **M1 live patch (splat)**: every brush stamp patches the store — the panel's
  four modes are **Carve** = a depth-limited cylinder scoop along the surface
  normal, **Add** = a dome, **Delete** = clear every cell in the brush ball,
  **Paint** = recolour the ball with the selected material. There is no
  bake/record edit path any more (the legacy `carve_edits.vxw` /
  `raise_edits.vxw` layers are read-only leftovers). Subtractive modes respect
  the water level: a scoop aimed below the plane is refused and no `Clear` edit
  ever touches a cell whose centre is below `WATER_LEVEL`. `ChunkStore::rebuildDirty`
  re-derives only the edited
  block region (edit AABB ± 12 cells, snapped to 8³ blocks; untouched blocks
  are copied verbatim — a unit test pins the localized result byte-equal to a
  full chunk rebuild). `LiveEditor` keeps per-chunk surfel caches and refreshes
  only the ±3-cell neighbourhood, then `SplatPass::patchChunkSurfels` writes
  the updated run into the **paged** surfel buffer: each chunk owns a slot run
  with reserved capacity (`m_chunkStart/Count/Cap`) so untouched chunks stay
  put; a chunk that outgrows its slot relocates into the opaque free area, and
  the buffer grows (tail regions shift) if needed. Patched chunks drop their
  micro tail + LOD ring until the next full upload. A freshly GPU-seeded chunk
  is the *pre-edit* run, so `stamp()` always refreshes the edited region on
  top of the seed (without it the first stamp in a chunk looks like a no-op).
  The hover **preview** tints the affected splats before the click (bind 13
  `BrushUBO`, tested against the surfel centre + 0.06 m skin, `bFlags.x`
  clipping subtractive brushes at the water plane; see
  [Rendering & GPU contract](rendering.md#edit-brush-hover-preview-splat-backend)).
- **M2 live patch (SVO)**: SVO handles are chunk-local. `GpuWorld::chunkInfo`
  (uvec4 per chunk = nodeBase/childBase/brickBase, shader binding 11) travels
  with the five world SSBOs, and the shader resolves every payload/handle/
  brick access through its owning chunk's base (`common_svo.glsl`
  `chunkBases`). `SvoPass::patchChunk` re-uploads a rebuilt pool into reserved
  per-chunk regions, relocating and growing the arrays as needed, so both
  backends show the same edit in the same frame.
- **M3 persistence**: chunks touched by `apply()` are flagged `edited` and
  serialized (bricks + boxes + state) into a VXW v2 tagged section by the
  background `OverlayWriter` (temp + rename, debounced; a newer queue()
  supersedes an unstarted write). The app saves `assets/runtime_edits.vxw` on
  mouse release and restores it at startup / after every world reload. Because
  the file is not in `world.json`, the layer poll never reloads it — the app
  applies it explicitly (`App::loadStoreOverlay`). Hover/anchor picking uses
  `rayPickStore` so it follows edits. `uHeight`/water stay stale in the edited
  region until a full reload.
- **Drag painting**: holding LMB keeps stamping along the cursor (spacing =
  ¼ brush diameter) and each stamp patches both backends in the same frame.
  Steady-state stamp cost is ~5–25 ms (region-limited store rebuild + cached
  surfel refresh + GPU patch). First touch of a chunk seeds its cache from the
  GPU's current run (`SplatPass::readChunkSurfels`, base surfels only) instead
  of re-baking the chunk, which removed the multi-hundred-ms first-stamp
  hitch; the cache keys of GPU-seeded surfels are recovered from their
  positions. Headless: `VF_TEST_STROKE`.
- Tests: `tests/test_store.cpp` pins store/field sign + object-flag agreement,
  edit round-trips, rebuilt pool validity (shader-style traversal),
  localized==full rebuild equivalence, surface-density preservation, overlay
  round trips and store-based chunk surfels; `tests/live_edit_check.py`
  renders a close view of the edit in **both backends** with
  `VF_TEST_EDIT` and asserts a visible-but-bounded diff plus sane probes.

Next milestones (per the editing plan): store-native load (records → chunks
without the global field), semantic tags + procedural ops + shading
unification (M4), performance hardening — streamed/budgeted bakes, chunk
seeding without hitches (M5).

## World synthesis pipeline (`LayeredWorld::load`)

1. **Manifest parse** (`worldfile::loadManifest`) — layer list with roles,
   enabled flags; comments tolerated; unknown keys skipped.
2. **Merge** — every enabled non-packed layer is read and validated against
   expected meta `{WORLD, VOXEL, WATER_LEVEL, GRID_N}`. Records are deduped
   by lattice key `(x<<20)|(y<<10)|z`; **manifest order = priority, first
   wins a cell**. All non-disabled entries are kept in metadata even when
   their file fails to read.
3. **Classification** — the layer named/filed `landscape` fills per-column
   arrays `colTop` (highest record y) + `colMat` (its material); every other
   enabled record becomes an *object cell* (key + material list).
4. **Oracle build** (`VoxelField::build`) — object cells are grouped into
   connected components; each component gets an exact-ish signed distance
   transform on its bounding grid (two 26-neighbour Dijkstra passes plus an
   exterior air flood for the sign), quantized to int8 and stored sparsely.
   Shells *and* enclosed interiors therefore read solid — no hollow holes.
5. **Presence grid** — global 4³-cell block mask of record cells ∪ object
   interiors drives octree subdivision.
6. **Per-chunk synthesis** — `hardware_concurrency` workers build each
   64³-cell chunk's octree recursively:
   - `classifyBox`: no geometry → empty root; entirely below the lowest
     terrain top → terminal solid handle; otherwise subdivide down to 8³
     bricks.
   - `fillBrick`: sample `VoxelField` per cell → solid words (exact record
     color where present, else palette), water words below `WATER_LEVEL`,
     air words (distance to nearest surface) above.
7. **Deterministic merge** — per-chunk pools are concatenated with handle
   offset adjustment; chunks without records get promoted to solid terminals
   when fully underground. Pools stay resident so later reloads only rebuild
   dirty chunks.
8. **Incremental rebuilds** — each layer's record AABB, enabled state and
   content signature are cached. On change, the union AABB (+ margin of 2
   chunks) marks the dirty set. Landscape changes force a full rebuild;
   >1024 dirty chunks degrade to full as well.

`reloadIfChanged()` compares mtime+size signatures of the manifest and every
layer file; the app polls it every ~0.5 s. Chat/MCP-triggered edits call
`requestWorldReload()` instead, which reloads immediately. Either way
`applyWorldReload()` waits idle, swaps the five SVO SSBOs and re-uploads the
terrain texture and shadow volume together.

## Frame loop

```
poll events → camera update → world poll (0.5 s) → picking (Ctrl+LMB hover/pick)
→ raymarch compute (offscreen rgba8 Image3D)
→ TAA resolve [interactive only, base blend 0.92, motion-adaptive]
→ blit to swapchain image
→ ImGui new frame → HUD/chat draw
→ vkCmdBeginRendering(LOAD op, swapchain view) → ImGui_ImplVulkan_RenderDrawData → vkCmdEndRendering
→ present
```

- 2 frames in flight; **one acquire semaphore per swapchain image** (fixes an
  NVIDIA+X11 present deadlock with per-frame-slot reuse).
- Present mode defaults to IMMEDIATE (`VF_PRESENT=mailbox` to try MAILBOX).
- Headless modes skip TAA, window events and present entirely; `--shot`
  dumps the offscreen image after frame 3, `--selftest` analyzes frame 30.
- Animation clock advances only interactively so headless shots stay
  deterministic; it reaches the shader as `push.misc.y`.

## Threading model

| thread | work |
|---|---|
| main | window, Vulkan submission, HUD, picking, world poll |
| synthesis pool (N = hardware threads) | chunk octree builds inside `LayeredWorld::synthesize` (joined before buffers swap) |
| chat worker | blocking streaming LLM request; results queued under a mutex, polled each frame by `ChatUi` |

`CellMap` and `VoxelField` are read-only during synthesis; only
`ChunkPool`s are thread-local. GUI persistence writes `world.json` from the
main thread between frames.
