---
title: Per-cell texture overrides (phase 2)
tags: [textures, surfels, svo, chunk-store, voxel-field, mcp]
sourceRefs: [src/voxel/worldfile.cpp, src/voxel/voxel_field.cpp, src/voxel/layered_world.cpp, src/voxel/chunk_store.cpp, src/voxel/surfelize.cpp, shaders/common_base.glsl, shaders/splat.vert, shaders/splat.frag, src/ai/mcp_server.cpp]
lastReviewed: 2026-09-19
---

# Per-cell texture overrides (phase 2)

Phase 1 gave every *material* an optional atlas layer (see
[[concepts/texture-atlas]]). Phase 2 lets a single authored *cell* pick a
different atlas layer, so one object can mix wood and stone without a
material change. The byte that carries it is the `VoxelRecord`'s `reserved`
field — the 16-byte record's 14th byte, which was unused before.

## The chain, end to end

1. **Authoring** — `vf_mcp write_object` / `add_voxels` accept `"tex": N` per
   cell (`src/ai/mcp_server.cpp`: `RawVox.tex` → `parseRawVoxels`).
   `makeVoxelRecord(..., int tex)` stores it into `v.reserved`
   (`src/voxel/common.hpp`). `N` is an **atlas layer index**, 0 = no override.
2. **File** — `putRecords` writes `reserved` verbatim; both v1 and v2 readers
   must restore it (`src/voxel/worldfile.cpp` — `v.reserved = tail[1]`).
   *This was the first bug found: the v1 reader read `tail[4]` but only
   assigned `materialId`, silently dropping the byte.*
3. **Field** — `LayeredWorld` pushes `v.reserved` into `m_objTexs`, parallel to
   `m_objCells`/`m_objMats`, and passes all three to `VoxelField::build`.
   The component pass seeds `s.argtex[i]` from it, and the Dijkstra inherits
   it along with the material. The collect loop packs it into the object
   hash value: **bits 16..23** (`uint32_t(sdf) | mat << 8 | tex << 16`).
   `sample()` decodes `out.tex = uint8_t(v >> 16)`.
   *Second bug: bits 48+ do not fit the hash's `uint32_t` words — the value
   was truncated to zero. The free bits are 16..23.*
4. **SVO brick** — `wordsFromPacked` / `paletteWordHi` write the tex byte into
   **word1 byte 0** (`src/voxel/layered_world.cpp`). Word1 is
   `a | refl<<8 | rough<<16 | (mat|objFlag)<<24`; the `a` byte was an
   always-255 filler that nothing reads (the SVO shader takes albedo from
   word0, and the water flag lives in `mat_ao.w`).
5. **Store** — `ChunkStore::decodeCell` maps `out.tags = out.a` (the same
   byte), `encodeCell` writes `cell.tags` back, and the store surfel path
   emits `tan_aspect.w = float(cd.cell.tags)` (`src/voxel/surfelize.cpp:989`).
   The bake path emits `float(s.tex)` directly (`:516`).
6. **Shader** — `splat.vert` sets `vTex = s.tan_aspect.w` (location 9);
   `splat.frag` sets `gTexOv = vTex`; `sampleTex` picks
   `slot = gTexOv > 0.5 ? floor(gTexOv + 0.5) : uTex.matTex[mId].x`.
   `tex` is stored as a **raw integer** (not /255) precisely so the `> 0.5`
   test works.

## The overlay-schema bump

`assets/runtime_edits.vxw` (the persisted live-edit overlay) stores brick
words verbatim. Before this change byte 0 of word1 was uninitialized filler,
so a stale overlay carries garbage there — which now reads as random atlas
layers painted over restored geometry. `kOverlaySchema` is bumped 2 → 3 so
the mismatch is rejected cleanly (`applyEdited` already treats an unknown
schema as "ignore, not an error"). **`runtime_edits.vxw` is also not a
`world.json` layer** — it is loaded explicitly by `App::loadStoreOverlay`.
If it appears in the manifest, its garbage `reserved` bytes pollute the
object field directly.

## Gotchas

- **Atlas layers must be bound.** `tex: 3` samples *layer 3*, which is only
  filled if some `textures` entry has `mat: 3`. An unbound layer is neutral
  grey — the override fires but looks identical to the palette. Bind the
  layer in `world.json` before blaming the plumbing.
- **`writeManifest` rewrote the manifest bare**, dropping the top-level
  `textures` table (and any other key vf_mcp doesn't know about) on every
  `write_object`. It now preserves unknown top-level keys via a
  balanced-brace scan over the raw file text (`emitOtherKeys`).
- The water words (`waterHi`) had `255` in byte 0 — that would give every
  water cell `tags = 255`. Byte 0 is now 0 for water.
- `ChunkStore::apply` reads the cell into a `StoreCell` and writes it back
  via `encodeCell`; it must decode `tags` from the word (like `decodeCell`)
  or every edit silently zeroes the override. `Clear` resets `tags = 0`.
- Box-filled cells (`fillBoxCell`, promoted `Solid` chunks) keep `tags = 0`
  — bulk-filled regions never carry a per-cell texture by construction.

## Verification

- `tests/test_worldfile.cpp`: the reserved-byte roundtrip + the
  manifest-preserves-unknown-keys tests.
- `tests/texture_check.py` `[phase2]`: authors a 4×4×24 block with `tex: 3`
  via `vf_mcp`, renders it against the same block without the override at a
  fixed camera, and asserts the diff fraction is in [0.2%, 10%] — small
  enough to prove the change is confined to the object, large enough to
  prove the byte actually reached the shader.
- `VF_DUMP_CHUNK=N` (main.cpp) reads one chunk's base surfels back from the
  GPU and counts `tan_aspect.w > 0.5` — the fastest way to check whether the
  byte is on the GPU at all.
- `VF_SPLAT_DEBUG=5` renders `vTex` as a heat ramp (dark blue = 0, green =
  the value/8). Beware: the sky pipeline ignores debug mode, so sky pixels
  read as a false positive unless excluded.
