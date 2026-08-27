# Rendering & GPU contract

Authoritative sources: `shaders/svo_raymarch.comp`, `src/render/svo_pass.{hpp,cpp}`,
`src/voxel/world.hpp` (handle encoding), `src/voxel/layered_world.cpp` (baking).

## Descriptor bindings (set 0)

| binding | resource | format/layout | contents |
|---|---|---|---|
| 0 | `uOut` image2D | rgba8, writeonly | raymarch output (offscreen Image3D) |
| 1 | ChunkGrid SSBO | std430 i32[] | `GRID_N³ = 16³` root handles per chunk (`-1` empty) |
| 2 | ChildBase SSBO | std430 u32[] | per node: index of its 8 contiguous child handles |
| 3 | Payload SSBO | std430 u32[] | per node: validMask bits 0–7, solidMask bits 8–15 |
| 4 | Handles SSBO | std430 u32[] | flat child pool |
| 5 | Bricks SSBO | std430 u32[] | 2 words per voxel, 1024 words per brick |
| 6 | `uHeight` image2D | rg32f | terrain: R = top world Y, G = material/255 |
| 7 | `uObjVol` image3D | r8_snorm | coarse object-only SDF volume for shadows |
| 8 | SelectionUBO | std140, 32 B | `uSel` + `uHover` vec4s: xyz = voxel center world pos, w = active flag |

## Handle encoding (`world.hpp`)

Low 2 bits of a u32 handle select its kind; the rest is the index:

| value | meaning |
|---|---|
| `0b00` | node → index into `childBase`/`payload` |
| `0b01` | brick → index into `bricks`, in units of `BRICK_WORDS` (1024) |
| `0b10` | terminal fully-solid cell (`kSolidHandle = 0xFFFFFFFE`) — whole subtree below terrain |
| `0xFFFFFFFF` | empty terminal (`kEmptyHandle`) |

`chunkGrid[i] == -1` means chunk *i* has no geometry. Chunks entirely below the
lowest terrain top are emitted as solid terminals so underground rays terminate
in O(1).

## Brick word packing

Per voxel, two u32s (8³ = 512 voxels → 1024 words per brick):

```
word0 = r | g<<8 | b<<16 | sdfByte<<24
word1 = a | refl<<8 | rough<<16 | matOrObj<<24
        where matOrObj = materialId | (isObjectSurface ? 0x80 : 0)
```

- `sdfByte` is an **int8**: signed distance in cells. Decode as
  `raw * VOXEL` meters — *not* `raw / 127 * VOXEL`.
- **Bit 7 of the word1 material byte is the object flag**, set by the bake
  when the object field wins the cell. The shader's `isObjectSurface()` uses
  it to switch to SVO-gradient normals and brick materials instead of the
  heightfield path.
- Solid cells carry the exact record color when a record exists at that cell,
  otherwise the palette color/refl/roughness of the field's material.
- Air cells store palette-of-nearest-hit-material + positive distance.
- Cells below `WATER_LEVEL` get water volume words: tint rgb(0.06, 0.22, 0.28),
  alpha 255, refl 130, rough 25, **material id 9** (shader-only surface plane;
  these voxels never register a hit themselves).
- Empty-cell fallback inside `map()`: `max(-sdBox(p, cmin, cmax), VOXEL*0.5)`
  followed by a **6-step bisection** — both constants are load-bearing
  (tunneling/hollow artifacts if changed).

## Push constant block — `RaymarchPush` (128 B, `alignas(16)`)

Defined in `src/render/svo_pass.hpp`; mirrored by the GLSL `PC` block.

| vec4 | x | y | z | w |
|---|---|---|---|---|
| `camPos` | cam xyz | | | 0 |
| `camRight` / `camUp` / `camFwd` | basis vectors | | | 0 |
| `a` | tanHalfFov (fov = 60°) | aspect | offscreen extent X | offscreen extent Y |
| `b` | worldSize (102.4) | voxelSize (0.1) | gridN (16) | frameIdx % 1024 |
| `sunDir` | normalized direction **toward** the sun | | | 0 |
| `misc` | 0 (unused) | **animation time (seconds)** | 0 | 0 |

⚠️ The comment in `RaymarchPush` says "x = animation time" but the actual
contract — set in `main.cpp` and consumed everywhere in the shader — is
**`misc.y`** (wind-sheared grass, blade streaks, water ripples, foam pulse).
Don't "fix" either side casually; keep them in sync.

`a.w` is reserved (past freeze-ripples bug); don't repurpose it.

## Terrain sampling

- `heightAt(xz)` — bilinear over `uHeight` `.r` (top surface world Y), giving
  smooth analytic-style terrain from per-column records. CPU-side mirror:
  `VoxelField::smoothTerrainY` so baked bricks see the same surface (no
  stair-step divergence between bricks and heightfield shading).
- `heightMatNearest(xz)` — nearest-texel `.g` × 255 for the terrain material.

## Object shadow volume (`uObjVol`)

256³ int8 snorm texels over the whole world (**0.4 m/texel**). Encoded range
±1.26 m at ±127; `+127` = far away. Shadow rays march
`min(heightfieldDist, objDist(uObjVol))` so AI-placed objects cast shadows
without terraced-terrain self-shadowing. It is deliberately too coarse for
shading normals — use brick SDFs there.

## Raymarch pipeline summary

1. Ray vs world AABB; advance to entry.
2. Terrain sphere tracing against `heightAt()` (+ material from records).
3. Object traversal: chunk grid → octree nodes (payload masks skip subtrees)
   → brick SDF sphere tracing with the packed distances.
4. Water plane at `y = -0.9`: bidirectional hit test above/below; underwater
   sets `gUnderwater` absorption tint; bed-absorption skip when submerged.
5. Shading (single path): GGX specular + energy-conserving diffuse,
   multi-scale SDF AO driving a bent normal, sky-driven ambient (analytic
   Preetham-ish model), ground bounce, foliage translucency (mat 8),
   grass sprite cards + blade streaks near-field, two-scale heightfield
   normals. Sun/moon constants are plain GLSL; fog density 0.0012.
6. Post: ACES tonemap → vibrance → split-tone → gamma 2.2.

All geometry inputs come from bindings 1–7 — **no analytic scene constants
exist in GLSL by design**; adding any breaks the data-only invariant.

## TAA resolve (`taa_resolve.comp`)

AABB-clamped neighborhood history blend, no reprojection (camera motion is
handled by clamping). History blend factor 0.92 after the first frame;
disabled entirely in headless modes so shots are deterministic.

## Selection highlight

The app writes selected (strong warm) and hovered (faint) voxel centers into
the binding-8 UBO every frame; the shader edge-highlights those cells. Test
hooks `VF_TEST_SELECT=x,y,z` / `VF_TEST_HOVER=x,y,z` inject deterministic
picks for headless screenshot checks.
