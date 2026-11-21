---
title: Photoreal foliage cards (ground-bound photographic ground cover)
tags: [foliage, cards, textures, scenery, data-driven, render-flag, photoreal]
sourceRefs: [shaders/common_base.glsl, src/render/texture_atlas.cpp, src/render/texture_atlas.hpp, src/voxel/worldfile.cpp, src/voxel/worldfile.hpp, assets/textures/cards/cards.json, tools/pack_foliage_cards.py, tests/photo_cards_check.py, AGENTS.md]
lastReviewed: 2026-11-19
---

# Photoreal foliage cards

Small plants (grass, low groundcover) are **not voxels**. They are real
**raster cards** — one instanced quad per blade, drawn by the FurPass's raster
pipeline and composited through its accumulation targets — exactly the way
micro-object fur cards are drawn. The card's photo is a CC0 cutout packed into
the shared texture atlas; the procedural blade tufts remain as the bit-13-off
escape hatch. See [[concepts/texture-atlas]] for the atlas and
[[concepts/fur-cards]] for the hair-card pass this reuses.

### Why raster, not a surfel shading effect
An earlier version sampled the card texture inside `applyFlora()` on each
surfel fragment. That could never look photographic: the ground is a field of
0.1 m surfel disks, so the "grass" was painted onto blocky disks and read as a
mosaic. The raster path draws actual vertical quads with per-fragment alpha
and a G-buffer depth test, so each blade has its own silhouette and the voxel
grid no longer shows through. `FurPass` already owned the instanced-quad
pipeline, the five additive accumulation targets, and the composite that
turns summed alpha into `1-exp(-Σa)` coverage — the grass draw reuses all of
it, so there is no second pass. The only additions are a card SSBO (binding
31), `grass.vert`/`grass.frag`, and `voxel::buildGrassCards()`.

## What is where

- **Shared library** `assets/textures/cards/` — 5 packed 512² RGBA cards
  (`card_23_grass.png` … `card_27_nandina.png`). RGB = photo basecolor,
  A = silhouette. Produced by `tools/pack_foliage_cards.py` from the 18
  realistic CC0 3DTexel atlases (basecolor + opacity, mean-matched). These are
  a **shared material library**, so every scenery can resolve them; they are
  not scenery content.
- **`cards.json`** — the library manifest: `name -> atlas layer` and
  `name -> PNG file`. Adding or repacking a card is a data edit.
- **Atlas layers 23..27** — trailing layers after the 23 palette materials;
  `TexAtlas::kCardBase = 23`, `kLayers = 28`. No new descriptor binding: the
  existing `sampler2DArray` at binding 22 and `TexTable` at binding 23 are used.
- **Render flag bit 13** (`gRenderFlags & 8192`) — ON by default
  (`m_renderFlags = 511 | (1 << 13)`); `VF_RENDER_FLAGS=511` is the exact
  pre-card look. The GUI toggle lives in the Render panel's EFFECTS section.
  Bit 13 drives the raster grass SSBO (`App::rebuildGrassCards()`); the old
  shading-level `cardSample()` path in `applyFlora()` is deliberately
  disabled so the two cannot stack.
- **Card generation** (`src/voxel/grass_cards.cpp`) samples the terrain
  heightmap at ungridded random positions (a regular grid, even jittered,
  leaves a visible lattice), keeps cells whose ground material is bound,
  and emits crossed 0.24–1.0 m blades with independent per-channel tone.
  The shipped scenery produces ~304k cards (~19 MB SSBO). The render area
  must be the FULL frame when grass is present — clipping it to the alpaca's
  fur-object bounds silently drops every blade outside that small rectangle.

## Binding is DATA, never hardcoded

The governing rule (now in `AGENTS.md`): **no scenery item may be hardcoded in
C++ or GLSL.** No material id, layer name, card name, ground-texture binding,
or world position is a code special case. Code provides generic read-the-table
mechanisms; new sceneries and re-bound materials must work untouched.

The card binding chain:

1. The scenery's `world.json` authors a top-level table, e.g.
   `"foliageCards": [{"mat":0,"card":"grass"}, {"mat":8,"card":"jungle"}]`.
2. `worldfile::loadFoliageCardManifest(manifestPath, cardsJsonPath)` joins it
   with the shared `cards.json` and returns `(mat, layer, file)` triples.
3. `TexAtlas::load` decodes each referenced card PNG once into its layer and
   records `m_cardLayer[mat]`. `fillTable` writes it into `matTex[mat].w`
   (the previously unused 4th lane; the shader reads only `.x/.y/.z`
   otherwise). `VF_TEXTURES=0` skips the load and leaves every layer at -1.
4. The shader samples `matTex[heightMatNearest(p.xz)].w` — the layer bound to
   the **ground texture under the hit** — and knows no material or card by
   name. Cards stand on `heightAt(p.xz)`, vertically: the quad's plane contains
   world +Y and a horizontal tangent, so the plant grows up out of its texture.
   A value of -1 means no card, and the procedural path is used as fallback.

`writeManifest` preserves `foliageCards` verbatim: it rewrites only `version`
and `layers` and re-emits every other top-level key unchanged. Verified by an
md5-stable manifest across an app run.

## Escape hatch / fallback

- **bit 13 off** (`VF_RENDER_FLAGS=511`): the original procedural crossed cards
  (`tuftAlpha`), `floraField` tufts, and the legacy `mId`-based shading run
  byte-identically. The new photo-card block is skipped entirely.
- **bit 13 on, unbound ground**: falls back to the procedural path too — a
  scenery with no `foliageCards` table renders like the old renderer even with
  cards enabled. This is what the gate's "unbound" arm pins.
- **VF_TEXTURES=0**: cards are not decoded and no layer is bound, so the
  bit-exact textured-off hatch holds with cards nominally on.

## Only bound ground materials grow plants

Plants appear **only** where the scenery explicitly binds a ground material in
`foliageCards`; every unbound material reads -1 and stays bare. Sand (material
3, golden beach/dune) is deliberately **not** bound, so beaches grow no grass —
the shipped LakesideHamlet table binds only grass (0/1), soil/wet-soil (2/22),
foliage (8) and moss (18). A scenery that wants dune grass opts in with a data
edit; no code special case assigns plants to a surface type.

## Cards vs voxels

The directive is "never use voxels for grass and small plants". In
LakesideHamlet the pure small-plant voxel layers (`hamlet_reeds`,
`hamlet_reeds_far`, plus the mixed scatter `hamlet_nature`/`hamlet_garden`)
were disabled in `world.json`; trees/forest (woody geometry) stay. The
photographic cards are now the ground cover for every bound ground material.

## Measured look vs the reference

Compared at 1920² against a photoreal grassland reference (KCD2): high-frequency
edge energy 35.0 (ours) vs 37.2 (reference), green-pixel share 62 % vs 58 %,
straw/gold share 49 % vs 58 %, mean RGB (105,133,61) vs (108,121,67). So the
texture's detail density and palette are close; what still reads as synthetic
is the limited source art — the CC0 atlas has only five distinct blades,
composited into each card — and the voxel ground under the gaps. Sourcing a
photographic multi-blade cutout is the next real lever, not more post tuning.

## Light, opacity and live editing (2026-11-20)

**Light/shadow.** Cards already had `ndl` against the sun and a sky/AO term,
but no sun occlusion. A per-pixel 16-step march over `uHeight` + `uObjVol`
runs in `fur_composite.comp` and is applied only to the grass part of the sun
term. Fur and grass share the accumulator set, so `grass.frag:oE.w` tags the
grass card's raw `alpha*ndl` (fur leaves it 0) and the composite subtracts
that part from `A.y` before re-adding it shadowed. Bit 14 (ON by default,
`VF_CARD_SHADOW=0` for a clean A/B) gates it alongside bits 2 and 13.

**Opacity.** The Render panel's **Foliage opacity** slider (0..1, default 1,
`VF_FOLIAGE_OPACITY` headless) scales `alpha` in `grass.frag`, so every
accumulation term and the final `1-exp(-sumA)` coverage fade together; the fur
coat is untouched. The card's `Has foliage` checkbox in the Textures panel is
the binding itself: checked = a card is bound to that material.

**One modal for both pickers.** The Textures panel no longer expands an inline
wall of thumbnails per material. Both the photo-texture and foliage-card
galleries are one `Texture picker` modal opened by a per-material **Choose…**
button, with a name filter and a `(palette)` / `(none)` option. The modal calls
the same `setTextureBinding` / `setCardBinding` paths the old tiles did, so
the existing GUI apply tests still cover persistence and re-upload.

**Always dynamic, like emissive.** Neither the presence nor the root height
of a card comes from the baked heightmap. `buildGrassCards` only proposes
hash-stable x/z sites; `App::grassCardLayerAt()` reads the LIVE `ChunkStore`
column: the first solid TERRAIN cell below a bounded hint supplies the
material, per-cell texture override and root Y, and the atlas binding decides
the card layer. The height texture is just a search hint. Paint a grass
material and the region rebuild grows blades; paint it away and they vanish.
A low band (~0.8 m) of OBJECT cells just above the terrain rejects enclosed
ground — this is what removed the blades that poked through the LighthouseIsle
barn's `footing` floor. A tree canopy metres higher is deliberately ignored,
so grass under trees stays. Verified: a camera on the barn floor is
byte-identical with cards on vs off; grass through the doorway still shows.

**Incremental rebuild.** `buildGrassCards` places candidates with a hash of
the candidate index (not a sequential RNG), so a card's position is independent
of its neighbours. `App` caches the uploaded cards; a paint stroke accumulates
its xz AABB and `rebuildGrassCardsRegion()` erases/rebuilds only the cached
cards rooted in that rectangle, then re-uploads. `grassCardLayerAt()` reads the
LIVE height texture (`m_heightCpu`, patched by `patchHeightTexture`) and store
tags, never the baked field — otherwise painted grass would never grow cards.
Measured on the hamlet: full rebuild 305,679 cards / 756 ms; one brush region
221 removed + 221 added in 12.3 ms (~60× faster).

The LighthouseIsle binding was also corrected globally: its two grass ground
materials carried jungle/nandina cards; mat 0 now uses `grass` (mat 1 keeps
`nandina`, whose colour matched its coastal ground better) and mat 8 uses
`jungle`.

## Gate interaction with `test-textures`

`tests/texture_check.py` had two stale assumptions after the scenery move:
it looked for `assets/world.json`, and its `VF_TEXTURES=0` arm also lost the
foliage cards (their atlas slots are zeroed with the atlas), producing ~1%
of frame difference that the gate attributed to the atlas. It now renders
against a **sandbox copy** of the shipped scenery and pins `VF_RENDER_FLAGS=511`
(bit 13 clear) for every arm, so cards are absent throughout and the atlas A/B
is isolated. `test-textures` passes 2/2. `visual_check.py`'s manifest path was
moved to `scenery/<VF_SCENERY>/world.json` for the same reason.

## Gate

`tests/photo_cards_check.py <binary>` (stdlib only) renders house/hero/water
views and checks: (A) bit-13-off byte-identity vs a pristine
`VF_BASELINE_BIN` (both binaries must use the SAME `VF_SCENERY`), (B) an
ON-state signal, (B2) both splat and SVO backends, (B3) `VF_TEXTURES=0` makes
bit 13 a no-op, and (D) removing `foliageCards` from a temporary scenery copy
restores the procedural look — the direct proof the binding is data, not code.
