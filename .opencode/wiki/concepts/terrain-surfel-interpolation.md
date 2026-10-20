---
title: Terrain surfel interpolation (what the splat size can and cannot fix)
tags: [rendering, surfels, terrain, heightfield, splat, measurement, debug-views]
sourceRefs: [shaders/splat.frag, shaders/common_splat.glsl, shaders/common_svo.glsl, src/voxel/surfelize.cpp, src/voxel/surfelize.hpp, src/app/world/surfel_stream.cpp, tools/surf_spectrum.py, tests/test_surfelize.cpp, AGENTS.md]
lastReviewed: 2026-10-20
---

# Terrain surfel interpolation

The question (2026-10-20): the splat backend's terrain reads as a field of
discrete discs - "cobblestones" - while the SVO reference reads as one
surface. **Can a better interpolation come from smaller splats, or from
non-uniform splat sizes?**

Short answer: **no, and the measurement says why.** The splat *size* is not
the lever; the per-surfel *normal* is. The size sweep below moves the metric
by ~10 % in both directions; the normal field carries the artifact; and the
normal's cell-scale variation is inherent to the 10 cm heightfield, so a
truly better interpolation needs a finer lattice or a heightfield-projected
terrain - not more or smaller discs.

## The instrument

Two things make this measurable instead of impressionable:

1. **The splat debug views** (`VF_SPLAT_DEBUG=n`): `1` = flat grey (pure
   coverage, no lighting), `2` = the surfel normal, `5` = the albedo, `13` =
   the heightfield residual. A flat view isolates the *geometry* from the
   *shading*: if the flat view is clean and the shaded view is not, the
   artifact is in the shading.
2. **The radial power spectrum of a terrain patch** (`tools/surf_spectrum.py`):
   the power per spatial-frequency band says *which scale* the visible
   structure lives at. A 0.14 m disk in the near field is 30-90 px, so the
   25-100 px bands are the surfel scale and the 6-25 px bands are the photo
   texture's grain.

Luma alone is a bad metric here: the sand's photo texture contributes
pixel-level noise that swamps the surfel-scale signal, and a disk-scale RMS
high-pass mixes in the large-scale relief.

## Measurement 1 - the structure is scale-invariant in the disk radius

Same camera (`pos -4.2, 0.9, 14.6`, heading 187 deg S), near-field sand
patch (670x140 px), power per band:

| render | 50-100 px | 25-50 px | 12.5-25 px |
|---|---|---|---|
| default (`VF_SPLAT_RADIUS` unset = 1.0) | 16.77 % | 6.66 % | 6.47 % |
| `VF_SPLAT_RADIUS=0.7` (disks 0.7x) | 15.54 % | 5.96 % | 6.13 % |
| `VF_SPLAT_RADIUS=1.4` (disks 1.4x) | 17.37 % | 6.21 % | 5.61 % |
| SVO reference (`--mode svo`) | 7.01 % | 2.76 % | 3.58 % |

Halving the disk radius moves the metric by 11 %, doubling it by 7 %, in
*opposite* directions and both far short of the 2.4x gap to the SVO. The
structure is **scale-invariant**: it is not a property of the disks.

**Why the size cannot help even in principle:** the source-over accumulation
of N smaller disks at proportionally smaller spacing produces the *same*
alpha profile as one big disk - the coverage fraction is unchanged. Smaller
disks only move the structure to a finer scale, they do not remove it. And a
*non-uniform* size changes the coverage, not the interpolation: it trades
holes for blobs.

## Measurement 2 - the structure is in the normal field

The same patch, but the debug views instead of the shaded image:

| view | 50-100 px | 25-50 px | 12.5-25 px |
|---|---|---|---|
| `VF_SPLAT_DEBUG=1` (flat coverage, no lighting) | 11.57 % | **0.21 %** | **0.43 %** |
| `VF_SPLAT_DEBUG=5` (albedo) | 11.37 % | 1.28 % | 1.22 % |
| `VF_SPLAT_DEBUG=2` (the surfel normal) | 20.59 % | **10.27 %** | **5.67 %** |
| the shaded image | 16.77 % | 6.66 % | 6.47 % |

The silhouette is clean and the material is flat. The **normal field carries
the artifact**, and the shaded image follows it.

## The cause

A surfel's normal is **one value for a whole 10 cm cell**, and the heightfield
is a staircase of 10 cm columns - so a cell at a step edge gets a mean
normal tilted by up to 45 deg while its neighbour does not. The rasterizer's
opaque base pass seals every covered pixel with the **nearest** fragment's
colour (`shaders/splat.frag`, `PASS_MODE == 4`), so the per-surfel shading
difference becomes a hard-edged patch at every cell boundary.

The SVO reference does not have this artifact because `calcNormal()`
(`shaders/common_svo.glsl`) evaluates the heightfield's analytic normal
**per fragment** - it is continuous over the surface.

A second, smaller dilution compounds it: the bake mixed the analytic
heightfield normal in *before* the neighbour smoothing
(`src/voxel/surfelize.cpp`), and the smoothing averages the cell's normal
with the **raw** normals of its face neighbours - so the analytic field
survived with a weight of only 1/(1+neighbours) (~14 %), and the noisy
per-cell voxel normals dominated.

## What landed

### 1. The terrain surfel is a heightfield sample (`SurfelParams::terrainAnalytic`, default ON)

A terrain top cell is a sample of a **heightfield**, not a voxel face. The
surfel is now placed ON the heightfield surface (the cell's top face, which
is exactly what the GPU height texture stores at that texel) and oriented by
the analytic heightfield normal - applied **after** the neighbour smoothing,
so the analytic field keeps its intended weight instead of being averaged
away.

The disk is therefore *tangent to the interpolated surface* rather than a
facet of the voxel it was sampled from, and the splat geometry agrees with
the height texture the water shading, the SSR and the heightfield-residual
debug all read.

Measured on the hamlet (near sand, 5 regions): the mean |laplacian| of the
luma (the edge energy) drops **3-7 %** in every region
(sand_fg 8.33 -> 7.81, sand_mid 28.16 -> 26.13, grass 42.32 -> 40.31). The
cost: 13-49 % of the pixels move by >12 codes - it is a visible change, and a
correct one.

The live/store path mirrors it exactly (the same flag, the same order) - the
flag lives in `SurfelParams`, so every path (bake, live store, GPU seed)
inherits it and cannot disagree. `VF_TERRAIN_ANALYTIC=0` restores the old
behaviour; `VF_SURFEL_HFMIX=<0..1>` sets the analytic field's weight.

### 2. The per-fragment terrain normal (render flag bit 9, default OFF)

`shaders/splat.frag` can shade a terrain fragment with the **continuous**
heightfield normal (`splatHeightNormal()`, the same formula the SVO uses)
instead of the surfel's own. The surfel keeps its geometric role - the
coverage stamp - but it no longer dictates the shading normal.

The gate is geometric, with no material list:

* the fragment's height agrees with the height texture at its xz
  (`abs(heightAt(q.xz) - q.y) < 0.25`);
* the surfel's **own** normal agrees with the heightfield's normal
  (`dot(n, hn) > 0.7`) - a vertical surface standing on the terrain (a grass
  card, a fallen log, a reed stem) is never flattened into the ground.

The shadow *gate* stays on the surfel's own normal: the shadow was baked with
it, and re-evaluating the gate per fragment would tear the terminator along
the surfel boundaries.

Flag: `VF_RENDER_FLAGS=1023` turns bit 9 on (511 = the shipped default).

**Measured neutral on the hamlet's terrain** (the edge energy and the
spectrum do not move; 9 % of the pixels change), and it costs eight texel
fetches per terrain fragment on a fill-bound path - so it ships **off**. It
is the mechanism a heightfield-projected terrain would build on.

## The measurement trap on the way (worth keeping)

The first version of the shader gate was *only* "the fragment is within
0.25 m of the heightfield", with no normal test. It measured **-20 % / -25 %**
on the cell-scale bands - a much better number than anything else here.

It was an artefact. The loose gate caught the **props** (the fallen branches
and stones lying on the sand, within 0.25 m of the heightfield) and shaded
them with the ground's normal: their contrast vanished, so the measured
structure dropped. Every *guarded* variant (the object test, the foliage
test, the normal-agreement test) keeps the props and restores the number. A
metric that improves when the scene is flattened is a broken instrument -
compare the diff map, not only the metric.

## What a real fix would have to do

The cell-scale normal variation is inherent to the **10 cm heightfield**: the
analytic normal is continuous, but it is not *flatter* (the bilinear
surface's kinks are at the cell scale). So a genuinely smoother terrain needs
one of:

* a **finer lattice** (5 cm) - the known cost gate, see
  [[concepts/detail-pipeline]] phase 5 (4x the surfels, +1.5-1.8 GB VRAM,
  +120 s load);
* a **heightfield-projected terrain surfel** - the same trick the water
  plane uses (`shaders/splat.frag`, the water branch intersects the analytic
  plane per fragment and shades it identically no matter which disk covered
  it): a terrain surfel would become a coverage stamp whose fragment lands on
  the interpolated heightfield instead of on its own flat disk plane. That is
  the one change that removes BOTH the faceting and the per-surfel shading
  at once, and it is a real pipeline change (the depth prepass and the base
  seal must use the same projected depth).

## Cross-refs

[[concepts/detail-pipeline]] (the surfel layout and the LOD rings),
[[concepts/splat-base-seal]] (why the nearest fragment's colour seals a
pixel), [[concepts/splat-edge-fade-measurement]] (the measurement discipline
this page follows), [[concepts/live-edit-surfel-parity]] (the store path
mirror), [[concepts/sky-probe-is-a-camera-assertion]] (a gate that fails for
a reason other than the one it names), [[entities/svo-render]] (the
reference backend).
