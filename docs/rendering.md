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
- **Baked per-surfel**: binary sun shadow (exact cell-DDA near field +
  sphere-trace far field over `VoxelField::sample`, same verdict as SVO
  `softShadow`, then blurred over face neighbours for a 1-cell penumbra
  instead of disk-shaped scallops) and bent-normal AO (`aoBake`, same rings
  as `splatAO`). Foliage (mat 8) gets 1.5× disks to close sparse-canopy
  gaps. Sun comes from `SurfelParams::sunDir`
  (the app's `--sun`); a sun change needs a rebuild, same as geometry edits.
- Water grid (0.25 m) wherever terrain tops sit below `WATER_LEVEL`.
- Chunk bucketing (16³ + 1 `chunkRange` offsets) for per-chunk draws/culling.
- **Micro-detail** (`SurfelParams::microDetail`, app default ON via
  `VF_MICRO=0` to disable, unit-test default OFF): deterministic 0–2 child
  disks per base surfel that turn texture texels into real geometry with
  parallax/occlusion — meadow crumbs, soil pebbles, rock chips, bark relief,
  roof moss puffs + underside filler seals, canopy leaflets. Children inherit
  the base cell's chunk, material and baked shadow/AO/bent (no extra marches)
  with a jittered tangent offset + micro-facet normal from a sin-hash of the
  lattice coords, so rebuilds stay bit-identical. All-layers count grows
  ~1.55 M → ~2.4 M.
- **Photoreal grade** (shared `common_base.glsl`, both backends in sync):
  `skyColor` adds an fbm cloud deck (thin at zenith so the sky probe stays
  blue) + golden-hour horizon warmth that tracks `kSunDir.y`; `detailAlbedo`
  tames the neon bake palette (olive meadows, loam, mossy shingles, pine)
  with large mottling + fine grain + per-material accents (log courses,
  roof moss, shore pebbles); `fogColor`/`mistFactor` add sun-warmed valley
  mist near the water table; water gets a two-lobe sun glitter + pebble
  sparkle. Foliage translucency/SSS kept modest (0.38/0.40) so canopies stay
  deep green instead of neon.

**Surfel layout** (64 B, 4×vec4, std430): `pos_rU`, `normal_rV`,
`bent_sh` (bent normal + baked shadow), `mat_ao`
(mat/refl/rough/AO + 2 for water). Footprints are isotropic (`rU == rV =
1.4·VOXEL`), so the vertex shader rebuilds the tangent frame from the normal.

**GPU** (`src/render/splat_pass.{hpp,cpp}`, `shaders/splat.{vert,frag}`):
dynamic rendering into the same `m_hdr`/`m_gpos` targets (plus a
`D24_UNORM_S8_UINT` depth image), so post/TAA/`--shot` work unchanged.
Five pipelines sharing one layout (surfel SSBO + `uHeight` + `uObjVol` +
16 B params UBO):
1. sky fullscreen triangle (no depth) → `skyColor` + hitType 0;
2. depth-only prepass (`PASS_MODE 3`), one `vkCmdDraw(4, n, 0, first)` per
   visible chunk: full disks write the nearest plane depth (a Hi-Z pyramid
   is built from it when occlusion culling is on);
3. opaque base (`PASS_MODE 4`): one draw over all chunks, depth test EQUAL
   against the prepass depth (bit-exact fixed-function depth, no shader
   write), no blend — the exact nearest fragment at every covered pixel
   writes opaque colour, so the sky can never bleed through the band's
   low-alpha disk rims. Early-Z drops every non-nearest fragment before
   shading;
4. opaque band (`PASS_MODE 1`): same draws, depth-tested with a dynamic
   `-VF_SPLAT_DEPTH_TOL` rasterizer depth bias (`vkCmdSetDepthBias`, LESS),
   no depth write, blended — the front surface's Gaussian coverage
   source-overs the base. Early-Z drops fragments deeper than the tolerance
   before shading;
5. water surfels (blended, depth-tested against the prepass, no depth
   write, `hitType 2`).
The resolve band keeps only fragments within `[nearest, nearest + tol]` of
the prepass depth, so the front surface band accumulates while far surfaces
inside the same chunk can no longer overdraw it in draw order (the old
dark-speckle failure mode). Because no pass writes `gl_FragDepth`, the
hardware still early-Z-rejects fragments behind the front surface, so
close-up overdraw stays bounded by the front band instead of every
overlapping disk (measured 55 → 5.9 ms `geo` at a near-tree 640×360 view).
Chunks still draw back-to-front (per-frame distance sort, ~100 us for
4096); within-chunk order among the resolved band is harmless because those
surfels sit on the same surface, and the opaque base removes the residual
rim translucency.

**Fragment**: exact ray/disk-plane intersect → fixed-function per-fragment
plane depth (from `gl_Position`, matching the prepass exactly; no
`gl_FragDepth` anywhere) → pure 2D Gaussian kernel
(`alpha = opacity·exp(−d2/(2σ²))`, defaults σ²=0.5, opacity=0.9; no opaque
core, no rim step) for soft filled silhouettes → `shadeSurfel` (twin of
`shadeTerrain` with baked sh/AO/bent + shared `applyFlora`) → fog → HDR +
G-buffer out. Overlapping disks on the same surface sum to a solid coverage
via source-over; the interior stays watertight through kernel overlap
(disk radius 1.4 cells, so every pixel sits well inside at least one
neighbour's peak).

**Cost drivers** (1080p hero, RTX 4090 Laptop): full per-fragment shadow/AO
marches measured ~13 ms — hence the CPU bake. Since the alpha rework the
opaque path is a depth prepass plus two passes over the same quads (opaque
base + Gaussian band), replacing the old core/rim split; because neither
pass writes `gl_FragDepth`, early-Z bounds the shaded fragments to the
nearest surface + resolve band, so moving close to geometry no longer pays
full overdraw for every overlapping disk twice (near-tree 640×360: `geo`
55 → 5.9 ms; 1280×720: 113 → 10 ms). Both backends remain within noise of
each other.

**Camera handling**: the vertex shader projects with honest `w = vz` (no
near-plane clamp — clamping smears behind-camera corners across the
screen as giant blobs when looking up past nearby geometry; the GPU clips
straddlers exactly). Backfaces collapse to zero-area quads, except foliage
(mat 8) + roof slabs (mat 7) which render two-sided (leaf shells are sparse;
stepped roof tops collapse below the eaves line and leave slits otherwise),
and except when a
per-frame CPU probe finds the camera embedded inside solid
(`sampleWorld(camPos).d < 0` → `setBuried`, `uSplat.x`), in which case
shells render two-sided instead of flashing sky.

**Tuning/debug**: `VF_SPLAT_SIGMA` (Gaussian variance, default 0.5),
`VF_SPLAT_OPACITY` (centre alpha, default 0.9), `VF_SPLAT_DEPTH_TOL`
(resolve band, default 0.002 ≈ 10 cm; applied as a dynamic rasterizer depth
bias), `VF_SPLAT_EXTENT`,
`VF_SPLAT_RADIUS` (disk multiplier, also live via hotkeys `[`/`]` which
drive the same uniform; 0.5–2.0, default 1.0),
`VF_SPLAT_NOCULL`/`NOWATER`,
`VF_SPLAT_DEBUG` (1 flat / 2 normal / 3 depth / 4 no-collapse shading /
5 facing / 6 albedo / 7 rough / 8 baked-shadow / 9 baked-AO / 10 hf-shadow /
11 objDist / 12 height-residual / 13 march origin / 14 Gaussian kernel mask /
15 edit-brush volume: magenta marks surfels whose centre lies inside the
active brush),
`VF_RENDER_FLAGS`, `VF_SURFEL_SMOOTH`
/`VF_SURFEL_HFBLEND` (bake variants), `VF_MICRO` (micro-surfel detail),
`VF_VOLFOG` / `VF_MOTIONBLUR` / `VF_DOF` (headless overrides for the J/K/L toggles).

## Photorealism chain (G/H/J/K/L, `App::recordPhotorealism`)

In-place LDR chain on `m_offscreen` after the post pass, shared verbatim by
the headless and interactive frame paths (interactive runs it before TAA so
TAA resolves the effected image):

1. SSR (`ssr.comp`, G / bit 5): G-buffer march with a proper camera
   projection, fresnel blend, water boosted (min 0.30 reflection).
2. SSAO (`ssao.comp`, H / bit 6): screen-space depth-difference AO —
   neighbours above the tangent plane occlude; darkens creases/contacts.
3. Volumetric fog (`volumetric_fog.comp`, J): low-altitude Rayleigh+Mie
   march; sky pixels reconstruct the camera ray.
4. Motion blur (`motion_blur.comp`, K): velocity from reprojecting `uGPos`
   with the previous frame's camera (`m_prevCam`, same source TAA uses);
   a still camera is a clean no-op. Extra push constants carry the prev
   camera (`MotionBlurPass::PC`, 208 B total).
5. DoF (`depth_of_field.comp`, L): CoC from G-buffer distance around
   `m_dofFocusDist` (10 m) / `m_dofFocalLength` (50); sky stays sharp.
   Focus params ride trailing push constants (`DepthOfFieldPass::PC`).

Gotchas fixed along the way, don't regress: every photo pass pipeline
layout's push range must cover its full PC block (a 56 B range with a 128 B
push silently feeds shaders garbage extents); descriptor-set binding counts
must match the shader (SSAO declared 2, shader uses 3 — writes vanished);
effect inputs must be the post-tonemap LDR image, not `m_hdr`.

## TAA resolve (`taa_resolve.comp`)

AABB-clamped neighborhood history blend, reprojected with the `uGPos`
world-position G-buffer plus the previous frame's camera (both backends
write `gpos`, so TAA works unchanged under splats). Base history blend
factor 0.92 after the first frame; disabled entirely in headless modes so
shots are deterministic. Shadow-border hardening: absolute AABB floor
(~3 LDR codes, dark shadows quantize coarsely) + motion-adaptive clamp
relaxation in uniform neighbourhoods (a moving shadow border would
otherwise reject history every frame = flicker) + velocity-driven blend
reduction and screen-edge fade to bound ghosting.

## Selection highlight

The app writes selected (strong warm) and hovered (faint) voxel centers into
the binding-8 UBO every frame; the shader edge-highlights those cells. Test
hooks `VF_TEST_SELECT=x,y,z` / `VF_TEST_HOVER=x,y,z` inject deterministic
picks for headless screenshot checks.

## Edit-brush hover preview (splat backend)

The edit tool (`C`) previews what the next LMB stamp would affect: while the
tool is active and a surface is hovered, every splat whose **centre** lies
inside the brush volume is tinted flat in the fragment shader (bind 13
`BrushUBO`: volume centre + radius, axis + half length, tint rgb + strength;
all-zero strength disables).

- volume = the exact cell set the CPU rasterizer will touch: **Carve** = the
  oriented cylinder based at the hit cell, `depth` long along the surface
  normal; **Delete/Paint** = the ball of `diameter/2` about the hit cell.
  The volume is grown by a 0.06 m skin so the surfels' `+0.05 m` emitter
  offset (pos = cell centre + n·0.05) stays inside; **Add** has no affected
  splats and shows nothing.
- subtractive brushes (Carve/Delete) set `bFlags.x`: nothing at or below the
  water plane is tinted, matching the app's rule that `Clear` edits never
  touch a cell below `WATER_LEVEL` (a scoop aimed at submerged ground is
  refused outright, with the held-back cell count in the log).
- tint: warm orange (carve), red (delete), the selected palette colour
  (paint, Material combo in the panel) at ~0.45–0.55 mix strength.
- testing a splat's *centre* (not the fragment position) keeps the highlight
  per-splat and matches the rasterizer's per-cell decision; the whole disk
  tints, boundary splats do not clip. Debug view `VF_SPLAT_DEBUG=15` shows
  the volume/mask directly.
- `VF_TEST_BRUSH="x,y,z,carve|delete|paint"` (+`VF_EDIT_DIAM`/`VF_EDIT_DEPTH`)
  activates the tool headlessly and renders just the preview, so the
  highlight is screenshot-testable (`tests/live_edit_check.py`). The SVO
  backend has no equivalent per-surfel tint; the tile path (`VF_TILE=1`)
  does not implement it yet.
