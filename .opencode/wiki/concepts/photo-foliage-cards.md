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

## Gate

`tests/photo_cards_check.py <binary>` (stdlib only) renders house/hero/water
views and checks: (A) bit-13-off byte-identity vs a pristine
`VF_BASELINE_BIN` (both binaries must use the SAME `VF_SCENERY`), (B) an
ON-state signal, (B2) both splat and SVO backends, (B3) `VF_TEXTURES=0` makes
bit 13 a no-op, and (D) removing `foliageCards` from a temporary scenery copy
restores the procedural look — the direct proof the binding is data, not code.
