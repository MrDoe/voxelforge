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
| `pos`, `rot` | **informational only** — where the baker placed the layer. Never applied at runtime; layer files hold absolute lattice coordinates. Note the JSON key is `rot` even though the struct member is `rotDeg`. |
| `enabled` | disabled files stay on disk but are excluded from merges |
| `listed` | runtime bookkeeping: `false` marks folder-discovered entries not yet persisted |

Parser notes:

- The minimal JSON reader tolerates `//` comments and skips unknown keys
  robustly (values may be any JSON value).
- `writeManifest` emits the canonical compact shape above.
- A fresh bake writes **only** `landscape` (+ `ai_edits` when non-empty)
  as enabled; everything else ships disabled for opt-in via the GUI panel.

## Layer merge semantics

`worldfile::readLayered(manifestPath, expectedMeta, out)` (used by tools/tests)
and `LayeredWorld::load()` share the same rules:

1. skip `role == "packed"` and `!enabled` entries;
2. read each `.vxw`, validate meta against `{WORLD, VOXEL, GRID_N}`
   (mismatch = hard error / layer skipped respectively);
3. dedupe by packed lattice key `(x<<20)|(y<<10)|z`, first claimant wins;
4. the layer whose `name == "landscape"` (or file `landscape.vxw`) additionally
   fills the per-column terrain arrays; all its other cells still participate
   in the normal dedupe.

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
                    float pos[3]; float rotDeg; bool enabled, listed; };

bool loadManifest(const std::string& path, std::vector<WorldLayer>& out);
bool writeManifest(const std::string& path, const std::vector<WorldLayer>&);
bool readLayered(const std::string& manifestPath, const WorldFileMeta& expected,
                 std::vector<VoxelRecord>& out);      // priority merge
}
```

## Related formats

- Brick word packing and GPU buffer layouts: [Rendering](rendering.md).
- Heightmap PNG: 2048×2048 16-bit grayscale, `h = -8 + u/65535 · 32` m
  (5 cm/texel), hand-rolled stored-deflate writer with
  `rowBytes = 1 + w*2` (`tools/heightmap_gen.cpp`).
