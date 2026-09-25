# World file format (VXW v1) & manifest

Authoritative implementation: `src/voxel/worldfile.{hpp,cpp}`.

## `.vxw` binary layout (little-endian)

```
┌──────────────┬──────────────────────────────────────────────────────────┐
│ Header       │ 64 bytes                                                 │
│ SVO buffers  │ optional legacy section (record-only layers leave it     │
│              │ empty — a valid state the loader accepts)                │
│ Voxel records│ N × 16 bytes                                             │
└──────────────┴──────────────────────────────────────────────────────────┘
```

### Header (64 B)

| offset | size | field |
|---|---|---|
| 0 | 4 | magic `"VXWF"` |
| 4 | 4 | version = 1 (u32) |
| 8 | 4 | `worldSize` (f32) — expected `102.4` |
| 12 | 4 | `voxelSize` (f32) — expected `0.1` |
| 16 | 4 | `waterLevel` (f32) — `-0.9` |
| 20 | 4 | `gridN` (u32) — expected `16` |
| 24 | 4 | `brickN` (u32) — `8` |
| 28 | 4 | CRC32 (poly `0xEDB88320`) of everything after the header |
| 32–63 | 32 | zero / reserved |

`worldfile::read()` rejects wrong magic/version and CRC mismatches
(unit-tested in `tests/test_worldfile.cpp`).

### Payload

Five length-prefixed u32 arrays (legacy SVO section), then the record list:

```
u64 count + i32[count]  chunkGrid   (GRID_N³ root handles; -1 = empty)
u64 count + u32[count]  childBase   (per node)
u64 count + u32[count]  payload     (validMask bits0-7 | solidMask bits8-15)
u64 count + u32[count]  handles     (flat child pool)
u64 count + u32[count]  bricks      (BRICK_WORDS = 1024 u32 per brick)
u64 count               voxel record count
VoxelRecord × count     16 B each
```

Record-only layers (everything the runtime consumes today) write zero counts
for all five SVO arrays. The merged-SVO "packed" role is legacy — loaders skip
such entries.

### VXW v2 (tagged sections)

Version 2 keeps the same 64 B header (with `version = 2`) and a section table
instead of the implicit payload, so extra streams can travel with a layer:

```
u32 sectionCount
per section:
  u32 type
  u64 byteLength
  u8  bytes[byteLength]
```

Known section types (`worldfile::kSection*`):

| type | name | contents |
|---|---|---|
| 0 | `RECORDS` | `u64 count` + `VoxelRecord[count]` (same 16 B records as v1) |
| 1 | `LEGACY_SVO` | the v1 five-array block |
| 2 | `STORE_CHUNKS` | opaque ChunkStore overlay (see below) |

Unknown section types are preserved verbatim in `WorldFileData::sections`, so
a read-modify-write round trip never drops data. v1 files stay readable;
`write()` emits v2 only when `WorldFileData::sections` is non-empty (record
layers keep the compact v1 layout).

The `STORE_CHUNKS` overlay is owned by `ChunkStore` and carries a schema
version (`u32 schema = 2`): `u32 chunkCount` followed by per-chunk
`{ u32 index, u8 state, u8 pad[3], i32 editAABB[6] (global lattice lo/hi),
u32 boxCount, u32 brickCount, boxes[boxCount] { i16 x,y,z,side; u8 mat;
u8 terrain }, bricks[brickCount] { u32 blockKey; u32 words[1024] } }`. The
edit AABB lets a reload re-refresh exactly the region the session touched
(GPU-seeded surfels + that region) instead of re-shading whole chunks. The app writes the live-edit overlay to
`assets/runtime_edits.vxw` (atomic temp+rename, asynchronously) and loads it
explicitly at startup / after every world reload; it is intentionally **not**
listed in `world.json`, so the layer poll does not reload it.

### VoxelRecord (16 B)

| offset | type | field |
|---|---|---|
| 0 | u16 | x lattice index |
| 2 | u16 | y lattice index |
| 4 | u16 | z lattice index |
| 6 | u8 | r |
| 7 | u8 | g |
| 8 | u8 | b |
| 9 | u8 | a |
| 10 | u8 | reflectivity (0–255 → 0–1) |
| 11 | u8 | roughness (0–255 → 0–1) |
| 12 | u8 | materialId (`kPalette` id 0–8; 9 is the shader-side water material) |
| 13 | u8 | reserved |
| 14 | u16 | zero padding |

Lattice → world mapping (see `VoxelRecord::position()`):

```
p = -worldSize/2 + (idx + 0.5) * voxelSize        // cell centers
lattice = 1024³ cells covering [-51.2, +51.2] m per axis
```

## Manifest `assets/world.json`

```jsonc
{
  "version": 1,
  "layers": [
    // order = dedupe priority: earlier layers win a cell ("first wins")
    { "file": "ai_edits.vxw", "role": "object",    "name": "ai_edits",
      "pos": [0, 0, 0], "rot": 0.0, "enabled": true },
    { "file": "house.vxw",    "role": "object",    "name": "house",
      "pos": [6.5, -0.40, 12.5], "rot": 0.0, "enabled": false },
    { "file": "landscape.vxw","role": "landscape", "name": "landscape",
      "pos": [0, 0, 0], "rot": 0.0, "enabled": true }
  ]
}
```

Field semantics:

| key | meaning |
|---|---|
| `file` | relative to the manifest's directory; **required** |
| `role` | `"landscape"` \| `"object"` \| `"scatter"` \| `"packed"`. Only one landscape layer should exist; `packed` entries are skipped by all loaders (legacy merged cache). |
| `name` | human id used by GUI/MCP (`enable_layer` matches name *or* file) |
| `pos`, `rot`, `rotX`, `rotZ` | **Applied at runtime** to `object`/`scatter` files. `transformRecords()` rotates the authored records about their exact bottom-center using `Ry(rot)·Rx(rotX)·Rz(rotZ)`, then applies `pos`; the placement hash dirties both old and new occupied chunks. All-zero is the authored lattice pose. JSON `rot` maps to C++ `rotDeg`; angles are degrees and are not restricted to `[0,360]`. |
| `enabled` | disabled files stay on disk but are excluded from merges |
| `listed` | runtime bookkeeping: `false` marks folder-discovered entries not yet persisted |

Parser notes:

- The minimal JSON reader tolerates `//` comments and skips unknown keys
  robustly (values may be any JSON value).
- `writeManifest` emits the canonical compact shape above.
- A fresh bake creates the terrain shell while preserving enabled
  `ai_edits.vxw` and foreign manifest entries whose `.vxw` files still exist;
  authored `hamlet_*` object layers are data, not baker-owned geometry.

The interactive trackball composes each ring drag on the right of the current
absolute pose (`Rnext = Rcurrent * Rlocal(axis, angle)`), so the object rotates
around its own local X/Y/Z axis. `placementEuler()` converts that local-axis
pose back to the manifest's `Ry(yaw)·Rx(pitch)·Rz(roll)` representation only
for persistence; the preview uses the resulting exact relative matrix. Ring
release only stages the pose: the explicit **Apply rotation** button writes the
manifest, while **Cancel** discards it. Move mode uses the same staged flow for
a constrained world X/Y/Z `pos` delta; the scene exposes all three colored
axis handles, and **Apply move** persists the selected delta.

## Runtime owner IDs and provenance

Owner IDs are derived at load time and are **not manifest fields**. For every
enabled `object`/`scatter` file, `LayeredWorld` chooses a stable ID in 1–254
(FNV-style filename preference, deterministic collision probing, and previous
IDs retained across toggles/reloads). The winning file ID travels with the
merged cell through `VoxelField::Sample::layer`, `PickHit::layer`, the sparse
`ChunkStore`, and full/live surfelization, including micro children. ID 0 means
terrain or live-added geometry with no base-file owner and is intentionally not
eligible for layer rotation.

This chain makes editor interaction exact: a plain LMB click in Rotate mode
reads and activates the picked cell owner, while `layerBox(file)` supplies only
the projected centre and bounds radius of its trackball. The shared 80-byte
surfel contract packs the owner into
`mat_ao.w` as `AO + 8 + 16*ownerId`; see [Rendering](rendering.md#selected-layer-rotation-preview).

## Layer merge semantics

`worldfile::readLayered(manifestPath, expectedMeta, out)` (used by tools/tests)
and `LayeredWorld::load()` share the record-level rules:

1. skip `role == "packed"` and `!enabled` entries;
2. read each `.vxw` and validate meta against `{WORLD, VOXEL, GRID_N}`;
3. apply `pos` and **all three** angles to `object`/`scatter` records;
4. dedupe by packed lattice key `(x<<20)|(y<<10)|z`, first claimant wins.

`LayeredWorld` then adds terrain-column classification, sparse object
provenance, AABBs/pivots/owner IDs, SVO synthesis, and dirty-chunk tracking.
Its first claimant also supplies the owner ID propagated to picking and
surfels; plain `readLayered()` intentionally returns records only.

## `worldfile` API reference

```cpp
namespace vf::voxel::worldfile {
struct WorldFileMeta { float worldSize, voxelSize, waterLevel; uint32_t gridN, brickN; };
struct VoxelRecord { /* table above */ glm::vec3 position(const WorldFileMeta&) const; };
struct WorldFileData { WorldFileMeta meta;
                       std::vector<int32_t> chunkGrid;
                       std::vector<uint32_t> childBase, payload, handles, bricks;
                       std::vector<VoxelRecord> voxels; };

bool write(const std::string& path, const WorldFileData&);
bool read(const std::string& path, WorldFileData&);   // validates magic+version+CRC

struct WorldLayer { std::string file, role, name;
                    float pos[3]; float rotDeg, rotX, rotZ; bool enabled, listed; };

bool loadManifest(const std::string& path, std::vector<WorldLayer>& out);
bool writeManifest(const std::string& path, const std::vector<WorldLayer>&);
bool readLayered(const std::string& manifestPath, const WorldFileMeta& expected,
                 std::vector<VoxelRecord>& out);      // placed priority merge

enum class PlacementAxis : uint8_t { X, Y, Z };
glm::mat3 placementRotation(float yawDeg, float pitchDeg, float rollDeg);
glm::vec3 placementEuler(const glm::mat3& rotation);
glm::mat3 rotatePlacementLocal(const glm::mat3& current,
                               PlacementAxis axis, float degrees);
glm::mat3 relativePlacementRotation(float oldYawDeg, float oldPitchDeg,
                                    float oldRollDeg,
                                    float newYawDeg, float newPitchDeg,
                                    float newRollDeg);
bool recordBottomCenter(const std::vector<VoxelRecord>& src,
                        const WorldFileMeta& meta, glm::vec3& out);
void transformRecords(const std::vector<VoxelRecord>& src,
                      const WorldFileMeta& meta, const glm::vec3& offset,
                      float rotDeg, float rotX, float rotZ,
                      std::vector<VoxelRecord>& out);
}
```

## Related formats

- Brick word packing and GPU buffer layouts: [Rendering](rendering.md).
- Heightmap PNG: 2048×2048 16-bit grayscale, `h = -8 + u/65535 · 32` m
  (5 cm/texel), hand-rolled stored-deflate writer with
  `rowBytes = 1 + w*2` (`tools/heightmap_gen.cpp`).
