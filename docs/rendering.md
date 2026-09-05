# Rendering & GPU contract

Authoritative sources: `shaders/svo_raymarch.comp`, `src/render/svo_pass.{hpp,cpp}`,
`src/voxel/world.hpp` (handle encoding), `src/voxel/layered_world.cpp` (baking).

## Descriptor bindings (set 0, SVO pass)

| binding | resource | format/layout | contents |
|---|---|---|---|
| 0 | — | — | intentionally absent (an unwritten binding invalidates the set) |
| 1 | ChunkGrid SSBO | std430 i32[] | `GRID_N³ = 16³` root handles per chunk (`-1` empty) |
| 2 | ChildBase SSBO | std430 u32[] | per node: index of its 8 contiguous child handles |
| 3 | Payload SSBO | std430 u32[] | per node: validMask bits 0–7, solidMask bits 8–15 |
| 4 | Handles SSBO | std430 u32[] | flat child pool |
| 5 | Bricks SSBO | std430 u32[] | 2 words per voxel, 1024 words per brick |
| 6 | `uHeight` image2D | rg32f | terrain: R = top world Y, G = material/255 |
| 7 | `uObjVol` image3D | r8_snorm | coarse object-only SDF volume for shadows |
| 8 | SelectionUBO | std140, 32 B | `uSel` + `uHover` vec4s: xyz = voxel center world pos, w = active flag |
| 9 | `uHdr` image2D | rgba16f, writeonly | linear HDR scene radiance |
| 10 | `uGPos` image2D | rgba16f, writeonly | G-buffer: xyz = world pos, w = hit type (0 sky / 1 solid / 2 water) |

The splat pass reuses bindings 6+7 (read-only) with its own set: 0 = surfel
SSBO (vertex), 1 = `uHeight`, 2 = `uObjVol`, 3 = 16 B params UBO
(vertex+fragment). HDR/G-buffer are dynamic-rendering attachments there,
not descriptors.

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
±1.26 m at ±127; `+127` = far away. `objDist()` returns **meters** (snorm ×
1.26); empty space reads exactly `+kObjVolMax` (no information beyond that
range — marches must skip, not occlude on, the clamp value). The SVO
reference uses exact DDA hits instead; the splat water path marches
`min(heightfieldDist, objDist)` with the clamp skipped. It is deliberately
too coarse for shading normals — use brick SDFs there.

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
6. Post (`post.comp`): HDR bloom → exposure → AgX tonemap (3 looks) →
   selection outline → gamma 2.2.

All geometry inputs come from bindings 1–7 — **no analytic scene constants
exist in GLSL by design**; adding any breaks the data-only invariant.

## Gaussian-surfel renderer (primary path, `--mode splat`)

One anisotropic 2D Gaussian disk per outer voxel-surface cell, rasterized as
instanced quads and composited as **opaque surfaces** (no sorting, no
transparency). `--mode svo` keeps the raymarcher above as the pixel
reference; `F` toggles interactively.

**CPU bake** (`src/voxel/surfelize.{hpp,cpp}`, rebuilt on every world reload
in `App::rebuildSurfels`, ~0.6 s for ~1.3 M surfels):
- Surface enumeration from the public `VoxelField` API only: terrain columns
  via `colTops()` (surface range `[minNbrTop, colTop]` per column), objects by
  scanning `objectBlockMask()` + `sample().obj`. Deduped, NaN-guarded
  (`safeNormalize`; NaN != NaN would silently break the determinism test).
- Normal = mean of outward (toward-air) face directions, blended toward the
  analytic two-scale heightfield normal on terrain tops, then one
  neighbourhood-averaging pass. Position = cell centre + n·VOXEL/2.
- **Baked per-surfel**: binary sun shadow (`shadowMarch` over
  `VoxelField::sample`, same verdict as SVO `softShadow`) and bent-normal AO
  (`aoBake`, same rings as `splatAO`). Sun comes from `SurfelParams::sunDir`
  (the app's `--sun`); a sun change needs a rebuild, same as geometry edits.
- Water grid (0.25 m) wherever terrain tops sit below `WATER_LEVEL`.
- Chunk bucketing (16³ + 1 `chunkRange` offsets) for per-chunk draws/culling.

**Surfel layout** (64 B, 4×vec4, std430): `pos_rU`, `normal_rV`,
`bent_sh` (bent normal + baked shadow), `mat_ao`
(mat/refl/rough/AO + 2 for water). Footprints are isotropic (`rU == rV =
1.1·VOXEL`), so the vertex shader rebuilds the tangent frame from the normal.

**GPU** (`src/render/splat_pass.{hpp,cpp}`, `shaders/splat.{vert,frag}`):
dynamic rendering into the same `m_hdr`/`m_gpos`
targets (plus a `D32_SFLOAT` depth image), so post/TAA/`--shot` work
unchanged. Three pipelines sharing one layout (surfel SSBO + `uHeight` +
`uObjVol` + 16 B params UBO):
1. sky fullscreen triangle (no depth) → `skyColor` + hitType 0;
2. opaque instanced quads, one `vkCmdDraw(4, n, 0, first)` per visible
   chunk (CPU frustum cull over chunk AABBs), depth test + write with
   per-fragment plane depth, alpha blend for the rim;
3. water surfels (blended, depth-tested, no depth write, `hitType 2`).
(No depth prepass: the main pass writes `gl_FragDepth`, which disables
early-z, so a prepass only ever changed rim blending — measured neutral to
negative. Per-fragment marches were the real cost driver; those are baked
on the CPU instead.)

**Fragment**: exact ray/disk-plane intersect → per-fragment *plane* depth
(`gl_FragDepth`, monotonic `1−exp(−t·0.02)` mapping; only relative order
matters since nothing else reads depth) → compact C1 kernel
`1−smoothstep(coreD2, 1, d2)` with opaque core `coreD2 = 0.9` (union of
cores tiles the plane; the thin rim is the analytic AA annulus, widened to
full opacity with distance) → `shadeSurfel` (twin of `shadeTerrain` with
baked sh/AO/bent + shared `applyFlora`) → fog → HDR + G-buffer out.

**Cost drivers** (1080p hero, RTX 4090 Laptop): full per-fragment shadow/AO
marches measured ~13 ms — hence the CPU bake. After baking: ~7.3 ms/frame
vs ~11.7 ms SVO.

**Tuning/debug**: `VF_SPLAT_CORE`
(coreD2), `VF_SPLAT_EXTENT`, `VF_SPLAT_NOCULL`/`NOWATER`,
`VF_SPLAT_DEBUG` (1 flat / 2 normal / 3 depth / 4 no-collapse shading /
5 facing / 6 albedo / 7 rough / 8 baked-shadow / 9 baked-AO / 10 hf-shadow /
7 rough / 8 baked-shadow / 9 baked-AO / 10 hf-shadow / 11 objDist /
12 height-residual / 13 march origin), `VF_RENDER_FLAGS`, `VF_SURFEL_SMOOTH`
/`VF_SURFEL_HFBLEND` (bake variants).

## TAA resolve (`taa_resolve.comp`)

AABB-clamped neighborhood history blend, reprojected with the `uGPos`
world-position G-buffer plus the previous frame's camera (both backends
write `gpos`, so TAA works unchanged under splats). History blend factor
0.92 after the first frame; disabled entirely in headless modes so shots
are deterministic.

## Selection highlight

The app writes selected (strong warm) and hovered (faint) voxel centers into
the binding-8 UBO every frame; the shader edge-highlights those cells. Test
hooks `VF_TEST_SELECT=x,y,z` / `VF_TEST_HOVER=x,y,z` inject deterministic
picks for headless screenshot checks.
