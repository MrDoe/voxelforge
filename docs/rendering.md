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
| 12 | `uGNorm` image2D | rgba16f, writeonly | G-buffer: xyz = shading normal, 0 on sky (water writes the flat plane normal: SSR/SSAO must never see ripple normals) |

The splat pass reuses bindings 6+7 (read-only) with its own set: 0 = surfel
SSBO (vertex), 1 = `uHeight`, 2 = `uObjVol`, 3 = 16 B params UBO
(vertex+fragment). HDR/G-buffer are dynamic-rendering attachments there,
not descriptors (three colour targets: `m_hdr`, `m_gpos`, `m_gnorm`; the
tile compute path writes the same three images through bindings 14/15/21).

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
contract — set in `src/app/frame/record_*.cpp` and consumed everywhere in the shader — is
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

One anisotropic Gaussian parent per outer voxel-surface cell, plus optional
small hard-edge crease bridges, rasterized as instanced quads and composited as
**opaque surfaces** (no sorting, no
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
- **Hard-edge fit + crease bridges** (`SurfelParams::edgeShrink` /
  `edgeFill`): only opaque object parents with two non-opposite exposed lattice
  faces are tightened. Normal disagreement may still shape anisotropy, but it
  never reduces object coverage; terrain, foliage, emissive materials, and
  explicit thin footprints keep their watertight radius. Each exposed face pair
  also emits one smaller tangent-aligned splat on the true face-plane
  intersection. Bridges inherit the parent's baked AO/shadow/bent normal,
  material, texture override, and owner ID, so they cost geometry but no extra
  shadow/AO march. Per chunk the stream is `[base | edge bridges]`;
  bridges stay in the always-on opaque range and add no draw
  call. The app exposes
  **Rendering → Sharp-edge fit** (`0.00–0.80`, default `0.35`) and **Interpolate
  crease splats** (default on); either change queues `requestWorldReload()` so
  full-bake and live-store paths rebuild together. `VF_EDGE_SHRINK` and
  `VF_EDGE_FILL=0/1` are launch overrides.
- **Baked per-surfel**: binary sun shadow (exact cell-DDA near field +
  sphere-trace far field over `VoxelField::sample`, same verdict as SVO
  `softShadow`, then blurred over face neighbours for a 1-cell penumbra
  instead of disk-shaped scallops) and bent-normal AO (`aoBake`, same rings
  as `splatAO`). Foliage (mat 8) gets 1.5× disks to close sparse-canopy
  gaps. Sun comes from `SurfelParams::sunDir`
  (the app's `--sun`); a sun change needs a rebuild, same as geometry edits.
- Water grid (0.25 m) wherever terrain tops sit below `WATER_LEVEL`.
- **Normals have exactly one rule.** A live stamp re-derives the edit AABB plus
  a margin, so any difference between the full bake and the store path is
  applied to cells the user never touched. `collectChunkCandidates` therefore
  runs the bake's own pipeline on store data: the raw normal is the mean of the
  outward directions of the *exposed* faces (`meanNormal`'s rule, out-of-lattice
  counting as air); a cell whose directions cancel to zero, or a thin corner of
  a thin object, is expanded into one axis-aligned entry per exposed face; every
  other entry averages its own normal with the raw normals of the face
  neighbours in range (`smoothNormals`). The store path must not derive a
  normal from a local SDF gradient: the brick SDF is byte-quantised and
  nearest-sampled, so a central difference cancels to exactly zero deep inside a
  thick body, and the old `+Y` fallback for that case pointed untouched walls
  straight up after an edit beside them. Measured on the cabin: 27.8 % of
  object surfels more than 30° off the bake (mean 22.4°, 1191 fabricated
  straight-up) before, 0.08 % (mean 0.57°, none) after. Gate:
  `tests/test_store.cpp` "live surfel normals follow the bake".
- Chunk bucketing (16³ + 1 `chunkRange` offsets) for per-chunk draws/culling.
- **Photoreal grade** (shared `common_base.glsl`, both backends in sync):
  `skyColor` adds an fbm cloud deck (thin at zenith so the sky probe stays
  blue) + golden-hour horizon warmth that tracks `kSunDir.y`; `detailAlbedo`
  tames the neon bake palette (olive meadows, loam, mossy shingles, pine)
  with large mottling + fine grain + per-material accents (log courses,
  roof moss, shore pebbles); `fogColor`/`mistFactor` add sun-warmed valley
  mist near the water table; water gets a two-lobe sun glitter + pebble
  sparkle. Foliage translucency/SSS kept modest (0.38/0.40) so canopies stay
  deep green instead of neon.
- **Enclosed space + light sources** (render-flag bit 8, default ON; the
  sun/sky path is a single directional light plus an *unoccluded* environment,
  so a cave lit only by AO still read as open-sky daylight). `skyVisibility*`
  casts ONE ray along `mix(bent, +Y, 0.65)`; a hit means `enclosed = 1`, which
  scales the analytic sky ambient by `mix(1, 0.16, enclosed)` and the IBL by
  `mix(1, 0.04, enclosed)`, and adds an albedo-scaled cave fill
  (`vec3(0.075,0.080,0.090) · enclosed · (0.35 + 0.65·ao)`) standing in for
  wall multi-bounce. The fill is not cosmetic: without it the removed daylight
  drops the `house` shot from 2.70 % to 7.21 % black-in-silhouette and trips
  `visual_check`'s 5 % hollow-voxel gate.
  Per backend: `lightVisibilitySvo` is a single `exactSVOHit`; the splat
  counterpart marches `heightAt` + `objDist` (its *sky* test deliberately uses
  `objDist` alone — `heightAt` calls every point below the terrain surface
  solid, which would mark hillside interiors as underground).
  Point lights come from a top-level `"lights"` array in `world.json`
  (`pos`/`color`/`radius`/`intensity`, max 16, non-positive radius or intensity
  dropped), parsed by `worldfile::loadLightManifest` into a 528 B std140 UBO at
  binding 25 and consumed by `applyLights()` in `common_base.glsl` — shared by
  the forward, GPU-cull and tile paths. `App::uploadLightSources()` is the only
  writer and must run after every pass `init()` (see the descriptor-ordering
  gotcha below). `VF_RENDER_FLAGS=255` disables the whole feature and restores
  the pre-lights image bit-exactly.

**Surfel layout** (80 B, 5×vec4, std430): `pos_rU` (w = radiusU, along the
stored tangent), `normal_rV` (w = radiusV, across), `bent_sh` (bent normal +
baked shadow), `mat_ao` (mat/refl/rough/packed metadata), `tan_aspect` (xyz =
in-plane unit tangent, w = per-cell texture override; zero tangent =
isotropic). `mat_ao.w` stores baked AO
plus either water (`AO + 2`) or a placed object owner
(`AO + 8 + 16*ownerId`, IDs 1–254). Owner ID 0 is terrain or unowned live
geometry. The metadata is packed only after AO smoothing; the shared
`common_surfel.glsl` helpers decode it consistently in the forward, GPU-cull,
and tile paths. Footprints start isotropic (`rU == rV = 1.4·VOXEL`); the
anisotropy bake stretches radiusU up to 1.6x along the local crease and stores
the tangent, and the vertex shader orthonormalizes it against the normal
(deriving a frame when zero).

**GPU** (`src/render/splat_pass.{hpp,cpp}`, `shaders/splat.{vert,frag}`):
dynamic rendering into the same `m_hdr`/`m_gpos`/`m_gnorm` targets (plus a
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
active brush; 16 owner mask: selected layer green, other object owners red,
terrain/water dark blue),
`VF_RENDER_FLAGS`, `VF_SURFEL_SMOOTH`
/`VF_SURFEL_HFBLEND` (bake variants), `VF_ANISO` (anisotropic footprint bake),
`VF_EDGE_SHRINK` (hard-edge parent reduction, launch default 0.35; GUI
**Rendering → Sharp-edge fit**) / `VF_EDGE_FILL=0` (disable derived crease
bridges), `VF_VOLFOG` / `VF_MOTIONBLUR` / `VF_DOF` (headless overrides for the J/K/L toggles),
`VF_SSAO_STRENGTH` (0..1, default 0.6), `VF_SSAO_RADIUS` (far-band world
radius in metres, default 0.8; the near crease band is 0.35x that),
`VF_SSAO_BLUR=0` (skip the cross-bilateral denoise), `VF_SSAO_DEBUG`
(1 = raw AO view, pre-filter, sky/water read 0; 2 = G-buffer normal view).

## Photorealism chain (G/H/J/K/L, `App::recordPhotorealism`)

In-place LDR chain on `m_offscreen` after the post pass, shared verbatim by
the headless and interactive frame paths (interactive runs it before TAA so
TAA resolves the effected image):

**Defaults**: SSR (bit 5) + SSAO (bit 6) + detail normals (bit 7) are ON by
default (`m_renderFlags = 255`). Measured cost on the hero view: 6.92 → 7.57 ms
at 720p, 9.99 → 11.33 ms at 1080p; `visual_check` keeps >15x headroom on the
black-in-silhouette gate (0.15-0.32 % vs 5 %). Volumetric fog (J), motion
blur (K) and DoF (L) stay opt-in. The fog was rewritten 2026-09-19: it now
marches only to the G-buffer hit distance (the old version marched a fixed
16 m from the camera regardless of the surface and darkened ~42 % of hero
pixels below luma 20), uses the correct forward-scatter sign with a
Henyey-Greenstein Mie lobe, bounded Beer-Lambert extinction and a
sky-ambient term that makes it aerial perspective. `tests/fog_check.py` pins
all of that; keep J off by default until the gate is green on your scene.
`VF_RENDER_FLAGS` overrides the whole mask.

1. SSR (`ssr.comp`, G / bit 5): G-buffer march with a proper camera
   projection, fresnel blend, water boosted (min 0.30 reflection).
2. SSAO (`ssao.comp` + `ssao_apply.comp`, H / bit 6): two-band world-scale
   AO. 6 directions x 3 steps per band march screen-space rays; each tap's
   world position (from the `m_gpos` G-buffer) is tested against the pixel's
   tangent plane, and the search radius is projected from metres
   (`ppm = 0.5*extentY / (tanHalfFov*z)`), so occlusion is measured in world
   units instead of pixels. Near band = 0.35x the far band (creases/contact),
   far band = `VF_SSAO_RADIUS` (grounding). Out-of-bounds, sky and water taps
   dilute the denominator (never occlude), and sky/water pixels pass through,
   so silhouettes and shorelines stay clean. The raw term goes to the
   `m_ssaoAo` scratch image; `ssao_apply.comp` then runs a 5x5
   cross-bilateral filter (view-depth + normal weights) and multiplies
   `base * (1 - strength*ao)`. Normals come from the `m_gnorm` G-buffer
   (finite-difference fallback if a backend ever stops writing it). Cost:
   ~1.2 ms at 720p / ~2.5 ms at 1080p on the hero view (two dispatches).
3. Volumetric fog (`volumetric_fog.comp`, J): Rayleigh+Mie in-scatter
   integrated along the view ray, terminated by the `m_gpos` hit distance
   (sky pixels march a fixed 140 m). Exponential density falloff from the
   water table × world-locked fbm patchiness, bounded Beer-Lambert
   extinction, sky-ambient scatter + forward Mie sun lobe. Self-contained:
   it needs no sky model, heightfield or object volume, so it keeps its
   3-binding layout (scene/gpos/out). Gate: `tests/fog_check.py`.
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
effect inputs must be the post-tonemap LDR image, not `m_hdr`. The splat
pipelines have **three colour attachments** — `createPipelines`, the three
`VkRenderingAttachmentInfo` blocks in `record`/`recordTile` and
`colorAttachmentCount` must always agree, or rendering silently breaks.
Per-frame scratch/target images are transitioned UNDEFINED -> GENERAL at
frame start (contents discarded); `m_gnorm` and `m_ssaoAo` follow the same
rule as `m_hdr`/`m_gpos`.

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

## Selected-layer rotation preview

Object ownership is provenance, not geometry by overlap. `LayeredWorld` gives
each enabled placeable `.vxw` a stable non-zero 8-bit ID (retained across
reloads/toggles, with filename-hash collision probing), tags the winning cell
in `VoxelField`, returns that ID in `PickHit`, and packs it into every base
surfel. In Rotate mode, one plain LMB click activates the exact picked
cell owner; it does not begin a drag. The placed AABB is then used only to
place the interaction surface: its projected centre is the trackball centre
and its farthest projected corner sets the radius. The outer circle is local
Y/yaw, the wide horizontal ellipse is local X/pitch, and the tall vertical
ellipse is local Z/roll. Press-drag one ring to stage a rotation; the object
remains active, and the explicit **Apply rotation** button commits it (or
**Cancel** discards it). The
screen projection mirrors `splat.vert` view-space math (`rel` dotted with the
camera basis and divided by forward depth)—never normalize `rel` first, which
moves the trackball away from the rendered object. Camera movement is applied
before picking, gizmo projection, and render-push construction so all three use
the same pose. Use ImGui's logical `DisplaySize` for both picking and gizmo
coordinates on high-DPI windows; GLFW cursor positions are logical pixels
while `framebufferSize()` is physical. Ring hit-testing is checked before
ImGui mouse capture, and the whole editor sidebar is created with
`ImGuiWindowFlags_NoInputs` while the pointer is on a ring (and faded to 0.30
background alpha during an active drag), so a ring overlapping the sidebar
cannot consume the drag or toggle an unrelated control underneath. Entering
Rotate no longer hides any panel: the editor is a single docked left sidebar
whose content pane swaps sections instead of opening and closing windows.

The preview pivot is the canonical source bottom-center plus the manifest
`pos`, not the current AABB centre. Each ring composes a local-axis rotation on
the right of the current absolute pose (`Rnext = Rcurrent * Rlocal`), then
converts back to the manifest Euler angles for persistence. If the committed
absolute placement changes from `Rold` to `Rnew`, the preview transform is:

```
Rdelta = Rnew * transpose(Rold)
p' = pivotPlaced + Rdelta * (p - pivotPlaced)
```

This is the same `Ry(yaw)·Rx(pitch)·Rz(roll)` pose that
`transformRecords()` will rebuild. Binding 14 (`RotUBO`) carries enabled state,
pivot, `Rdelta`, target owner ID, and an optional Move-mode translation lane.
The forward vertex stage, GPU-cull
compute stage, and tile bin/render stages all apply it only when
`surfelLayerId(mat_ao.w) == targetId`; terrain, water, unowned live geometry,
and other files do not move. While previewing, conservative CPU chunk culling
is bypassed so source-chunk distance/LOD
cannot make the selected owner disappear. The final transform remains active
until `applyWorldReload()` swaps in the committed rebuild.

`VF_TEST_ROTATE_LIVE="yaw,pitch,roll"` plus `VF_ROTATE_LAYER=<file>` is a
preview-only test hook and never writes the manifest. `VF_SPLAT_DEBUG=16`
renders owner classes directly: selected green, other objects red,
terrain/water dark blue. `tests/visual_check.py` renders the mask before and
after a synthetic cabin rotation and requires the green mask to move while the
red mask remains fixed. `VF_TEST_ROTATE` is the separate commit/rebuild hook. `VF_TEST_MOVE_LIVE="dx,dy,dz[,layer]"`
is the matching preview-only translation diagnostic and never writes the
manifest.

Move mode uses the same selected-owner GPU translation lane: it grabs the exact
owner, accumulates a delta on the selected world X/Y/Z axis, draws a small axis
handle, and writes `pos` only when **Apply move** is pressed.

## Edit-brush hover preview

The edit tool (`C`) previews what the next LMB stamp would affect: while the
tool is active and a surface is hovered, every surface sample inside the brush
volume is tinted flat in the fragment/compute shader (bind 13 `BrushUBO`:
volume centre + radius, axis + half length, tint rgb + strength; all-zero
strength disables).

`inBrushVolume` lives in `shaders/common_surfel.glsl` and is bound at the
**same binding 13 with the same std140 layout in both backends**, so one
declaration serves both pipelines. The splat path tests it against the
**surfel centre**; the SVO reference tests it against the **raymarch hit
point**, which is a closer match to the CPU rasterizer's cell set.

The preview follows the live pick when the pointer is in the scene and
otherwise falls back to the last scene pick (`App::m_latchedHover`), so
reaching for a sidebar slider does not make the preview blink out. It is a
preview-only feed: `applyEditLive` still requires a live hit, so a latched
preview can never stamp.

**Depth has its own marker, because the tint cannot show it.** The tint only
marks geometry *already in the scene*, and a Carve's extra depth lies below
the surface (solid material) while an Add dome grows into empty air — neither
has a surfel. Measured: the Carve affected-surfel mask was bit-identical
(2920 px) at depths 0.1/0.5/2.0/12.0 m. So `shaders/post.comp` projects the
hit and the volume's far end to screen space (same `dot(rel, basis) /
dot(rel, forward)` convention as `splat.vert`), strokes a line between them
and puts a ring on the far end. It is drawn on **every** fragment, not gated
on the visible-surface G-buffer, and both ends are clamped into the viewport:
a deep brush's far end is metres below the surface and would otherwise project
far outside the frustum. Depth 0.5 → 6 m now moves 1.75 % of the frame against
a 0.008 % noise floor. Because a screen-space line is bounded by the frame, the
clamped end keeps moving along the edge as depth grows and a short **cut-off
cap** is drawn across the axis whenever the true far end is off-screen, so 6 m
and 12 m stay distinguishable (3.0 %) instead of pinning to the same picture.
The sidebar footer also prints the depth numerically (`carve 15vox 1.5m`); a
number cannot saturate, so it is the readout that stays exact at every depth. The marker's hue is always the brush's own tint, so it
never contradicts the warm-Carve / green-Add assertions.

- volume = the exact cell set the CPU rasterizer will touch: **Carve** = the
  oriented cylinder based at the hit cell, `depth` long along the surface
  normal, reaching `EditableWorld::kCarveTopMargin` (0.2 m) above it (that
  margin is what opens the surface the scoop starts at, so the preview must
  include it); **Delete/Paint** = the ball of `diameter/2` about the hit cell.
  The volume is grown by a 0.06 m skin so the surfels' `+0.05 m` emitter
  offset (pos = cell centre + n·0.05) stays inside; **Add** = the dome itself
  (see below - the ground patch the dome buries is what tints). **Smooth** has
  no analytic cell set to draw: the terrain path relaxes column tops and the
  object path relaxes a surface coordinate in the picked surface's 2D plane,
  so its blue preview is a conservative ball around that footprint rather than
  a claim that every visible surfel is edited the same way.
- water-plane splats are never tinted (their `mat_ao.w > 1.5` flag): the plane
  is one fixed level that a carve exposes rather than removes, so showing water
  as "about to be cut" would be wrong.
- **Add** tints the growth footprint green: the volume is the exact extruded disk + rounded lip
  `makeDome` rasterizes (brush radius in `bVolume.w`, growth depth as a
  negative `bAxis.w`, with the skin folded into both), so the
  tinted splats are the ground patch the raised shell buries. The new shell has
  no splats yet, so the volume itself is the honest preview.
- **Smooth** calls `ChunkStore::makeSmoothEdits`, which dispatches on the
  picked store cell and plans the whole batch before the first mutation:
  - a **terrain** pick snapshots terrain column tops, applies one weighted
    local-height relaxation with a circular falloff, emits Clear/terrain-Set
    edits only for columns whose target changes, and protects object-owned
    columns (and any span that contains an object);
  - an **object** pick generalises the same relaxation to an arbitrary
    surface axis. The axis is the picked cell's *one-sided* face (solid on one
    side, air on the other; the SDF gradient breaks a thin wall's tie), the
    footprint is the circular disk in the plane perpendicular to it, and each
    row's surface coordinate relaxes toward the weighted mean of its 8 lateral
    rows, clamped to their min/max. The move is applied as one contiguous
    Clear/Set span along the axis, so a bump is removed whole in a single
    stamp (no unsupported floater), a notch fills, flat faces/walls/posts/
    staircases are fixed points, and a second stamp is a no-op. A short
    grounded bump contributes the ground level of its bare 3x3 ring; fills are
    always object-owned (`terrain = false`), so the terrain height texture and
    the water bed never change.
  The normal live path then refreshes the height texture, surfels, SVO pool,
  undo state, and overlay just like the other brush modes. Smooth refreshes
  its splats over `LiveEditor::kExactStampMargin` (12, the store's SDF band)
  instead of the default 3, because the rebuild re-solves normals/AO across
  that band; undo and overlay restore use the exact margin too.
- the volume test runs on the surfel *centre*, matching the CPU rasterizer's
  cell set; the empty run case skips the copy but must still zero the chunk's
  draw count (see `AGENTS.md` gotchas).
- binds 13 also backs the debug mask (view 15) and the headless
  `VF_TEST_BRUSH` hook; see `docs/tooling.md` for the env knobs.

## Undo and "Clear live edits"

Every live stamp records the pre-edit state of each cell it touches (first
occurrence per cell wins) in a per-stroke map; stroke end pushes that map as
one undo step (`App::finishStroke`, bounded to 250k cells / 32 steps - an
oversized stroke is dropped rather than becoming a partial undo). Undo
(`Ctrl+Z` / the panel button) replays those cells as Clear/Set edits through
`App::commitStoreEdits`, the same store -> LiveEditor -> GPU patch path a stamp
uses, so the geometry disappears in the same frame; the overlay is re-queued.
For a lowering stroke, `finishStroke` derives extra height-texture scan
headroom from the inverse Set cells relative to the post-stamp tops. This is
what lets undo restore a tall terrain spike, including after several drag
stamps, without leaving the water/bed texture stale. The refresh is
terrain-only (`c.solid && !c.obj`), while overlay restore and Clear scan from
the world top so arbitrary Smooth height changes cannot exceed their headroom.
A world reload flushes pending overlay writes and invalidates cell-state undo
history before re-adopting the store. The touched chunks then keep the live
path's store-derived shading until the next full rebuild (exactly like a
painted stroke).

"Clear live edits" is the nuclear option, in place (no reload needed for the
revert): flush the overlay writer (a pending save could otherwise land after
the delete and resurrect the file), delete the overlay, re-adopt the store from
the baked pools (`LayeredWorld::invalidateStore` -> `ChunkStore::adopt`), then
re-seed the touched chunks (surfels + SVO pool + height-texture window) and
request a full world reload so the bake's LOD rings and normals return. The
overlay path honours `VF_OVERLAY_PATH` (tests keep their edits out of
`assets/`).

Regression it pins: `SplatPass::patchChunkSurfels` used to ignore zero-surfel
patches (an empty vector's `data()` is null and tripped the `!data` guard), so
any chunk whose run became empty kept drawing the removed material - undo and
Clear looked like no-ops while the store was already correct.

## Water (splat backend): one fixed-level plane

The water is a single analytic plane at `kWaterLevel = -0.9` - there is no
per-column water state anywhere. In the splat backend it is rasterized as a
**world-wide 0.2 m grid of coplanar surfels** (`buildWaterSurfels`; one quad
per grid cell, ~263k at 513x513) appended after the opaque run and bucketed per
chunk for frustum culling:

- the grid is **pure coverage**: the fragment shader intersects the analytic
  plane per fragment and shades it there (`shadeWaterSplat`), so every water
  fragment on the plane is identical no matter which cell covered it - no disk
  borders, no per-cell look;
- the depth test against the opaque prepass does the clipping: cells standing
  over dry land, objects or a channel above the level are simply behind the
  opaque geometry and never shade. A dig below the level therefore shows water
  **automatically** - there is no flood pass and no water-buffer patching;
- the SVO backend intersects the same plane analytically, so both backends
  agree by construction.

What must stay in sync is the **height texture** (`uHeight`, rg32f = top world
Y + material) the water shading reads for the bed: shore foam, the absorption
alpha and the reflected-bed march all use `heightAt()`. `App::patchHeightTexture`
re-derives the edited columns from the runtime store (top solid cell + material)
and re-uploads just that sub-rect after every stamp and after an overlay
restore. Stale, a dug channel still read "land above the plane" and shaded as a
thin foam-washed sheet instead of the water used by the river.

Cost: the water plane's fill is ~0.2-0.45 ms at 640x360 (measured with
`--smoke` against `VF_SPLAT_NOWATER=1`); the world-wide grid is ~5% of the
surfel count and the depth test rejects the dry cells before shading.
`VF_SPLAT_NOWATER=1` skips the water-plane draw (A/B for the tests).

- tint: warm orange (carve), red (delete), the selected palette colour
  (paint, Material combo in the panel), and conservative blue (Smooth
  terrain/object footprint) at ~0.45–0.55 mix strength.
- testing a splat's *centre* (not the fragment position) keeps the highlight
  per-splat and matches the rasterizer's per-cell decision; the whole disk
  tints, boundary splats do not clip. Debug view `VF_SPLAT_DEBUG=15` shows
  the volume/mask directly.
- `VF_TEST_BRUSH="x,y,z,carve|delete|paint|smooth"` (+`VF_EDIT_DIAM`/`VF_EDIT_DEPTH`
  and optional `VF_SMOOTH_STRENGTH`) activates the tool headlessly and renders
  just the preview, so the
  highlight is screenshot-testable (`tests/live_edit_check.py`). The SVO
  backend has no equivalent per-surfel tint; the tile path (`VF_TILE=1`)
  does not implement it yet.
