---
title: Splat canopy parity - why the splat backend paints the tree black
tags: [splat, svo, shading, artifact, canopy, regression-guard]
sourceRefs: [shaders/splat.frag, shaders/common_splat.glsl, shaders/common_surfel.glsl, src/voxel/surfelize.cpp, tests/visual_check.py]
lastReviewed: 2026-10-21
---

# The "slots in the tree" artifact (2026-10-21)

The user reported *slots/slits* in the tree of the lakeside hamlet. The
splat backend showed them; the SVO reference did not ("there are no slits on
SVO reference"). They read as dark slivers in the canopy, which is what made
them look like slots in the ground under the tree.

## The locked repro

The camera was reconstructed from the HUD line in the user's own screenshot
(`Pos -4.2, 0.9, 14.6` / `Head 187 deg S`) plus the horizon height in the
frame. It reproduces the user's frame at **Pearson r = +0.74** against
`.opencode/tmp/invest/user_splat.png`:

```
--cam -4.2 0.9 14.6 -5.404 -0.488 4.792 --width 1280 --height 800
```

Two traps when comparing frames:

- The app's canvas is the **full window**; the sidebar is an *overlay* on its
  left ~334-346 px. A naive 16:9 comparison of the whole PNG (which is
  1280x810: a 10 px WM title bar + the canvas) misaligns by a fifth of the
  frame and reads as "uncorrelated" (r ≈ 0.03) for a perfectly good camera.
- The user's PNGs live in `.opencode/tmp/invest/` (1280x810). The
  `1600x932` PNGs in `.opencode/tmp/` are a *different* view.

## The instrument: the near-black mask

The metric is **splat-only near-black**: pixels below luma 30 in the splat
frame that are at or above luma 30 in the SVO frame. One-directional - the
SVO-only set is 0.20% of the frame, so the splat backend is painting black
where the reference paints mid-tones.

| frame (1280x800, with the user's overlay) | mask | mean luma at the mask | SVO at the same px |
|---|---|---|---|
| splat | 76,268 px (7.45%) | 19.2 | 62.9 |
| splat, render bit 8 off | | 25.6 (68% still <30) | |
| splat, bits 8+1 off | | 36.0 (50.8% still <30) | |
| SVO | - | - | 62.9 (0% <30) |

At the test's own resolution (640x400, `VF_NO_OVERLAY=1`): **21,785 px
(8.51%)**, mean luma 19.9 vs the reference's 60.7.

The masked pixels are **warm dark brown** ((26,14,2), (31,17,2), (36,20,3))
where the SVO shows neutral grey-brown ((70,61,60), (54,44,39)) - the same
material family, a very different brightness. Not a coverage hole: a hole
would show sky or the background, and the SVO shows *lit geometry* there.

## What causes it (ranked by measurement)

**THE MECHANISM (found last, and it is the seal).** The ownership mask
(`VF_SPLAT_DEBUG=16`) classifies the artifact pixels: **19,741 of 21,703
(91%) are OBJECT pixels** - the tree - where the SVO reference shows *lit*
geometry of the same tree. So the splat path is painting the canopy's inner,
shadowed leaves over the lit front leaves.

The base seal writes the **nearest** fragment's colour at its own kernel alpha
(above the `VF_SPLAT_SEAL_ALPHA` threshold), and "nearest" is decided by the
depth prepass on the **disk plane** depth. A flat disk of radius 1.4 cells
over-covers its neighbours, so a dark inner leaf's *plane* can be nearer than
the lit front leaf's plane over the pixels the lit leaf should own - and the
seal hands the whole pixel to the dark one. The band pass cannot repair it:
the seal already wrote the pixel's base colour.

`VF_SPLAT_SEAL_ALPHA` is a clean monotone dial over that (locked camera,
640x400, mask = splat<30 AND svo>=30):

| seal threshold | mask px | mask % | luma at mask | mean luma | IoU vs SVO |
|---|---|---|---|---|---|
| 0 (seal everything, the default) | 21,703 | 8.48% | 19.8 | 93.4 | 0.8955 |
| 0.25 | 21,703 | 8.48% | 19.8 | 93.4 | 0.8955 |
| 0.5 | 13,320 | 5.20% | 20.3 | 100.1 | 0.9298 |
| 0.75 | 7,922 | 3.09% | 21.6 | 103.2 | 0.9519 |
| 1.0 (never seal) | 5,316 | 2.08% | 23.1 | 104.0 | 0.9626 |
| SVO reference | - | - | - | 92.1 | (reference) |

Relaxing the seal collapses **75%** of the artifact and moves the silhouette
*closer* to the reference (0.8955 -> 0.9626); the cost is a frame ~13%
brighter than the reference (104.0 vs 92.1), because the surfaces accumulate
more light once nothing is sealed at its kernel alpha.

The same dial at **1280x800** with `VF_CHAOS_SHRINK=0` (so the seal is
isolated; SVO frame mean 90.89, SVO at the mask 62):

| seal | mask px | luma at the old mask | frame mean | sky-mask IoU | new sky vs default |
|---|---|---|---|---|---|
| 0 | 94,116 | 18.65 | 89.66 | 0.4800 | 0 |
| 0.5 | 57,779 | 41.87 | 97.63 | 0.4796 | 6,786 |
| 0.75 | 35,198 | 54.81 | 101.25 | 0.4782 | 9,813 |
| 1.0 | 24,372 | 59.23 | 102.17 | 0.4779 | 10,268 |

Mask luma reaches the reference (59.2 vs 62) but the **whole frame** ends up
+12.4% brighter than it (102.2 vs 90.9) and 10,268 px open to sky. So a global
unseal is a trade, not a fix: it is the argument for changing **which fragment
the seal picks**, not for turning the seal off. (The coverage count that
[[concepts/splat-base-seal]] proposes is the wrong instrument for that - see
"Why the coverage count in the stencil does not work" below.)

### The band tolerance, and the reframing of the mechanism

`VF_SPLAT_DEPTH_TOL` (the band pass's depth bias, default 0.002 ~ 10 cm) is
the *other* lever, and a much better trade than unsealing. On the clean
baseline (the plane depth, 640x400; the 0.002 row equals the pre-experiment
baseline, so the revert is confirmed):

| depth tol | mask px | mask % | luma at mask | frame luma | sky % |
|---|---|---|---|---|---|
| 0.002 (default) | 21,784 | 8.51% | 19.9 | 93.2 | 0.76% |
| 0.01 | 19,130 | 7.47% | 20.3 | 94.8 | 0.75% |
| 0.02 | 16,324 | 6.38% | 20.7 | 95.8 | 0.74% |
| SVO reference | - | - | - | 92.1 | 1.59% |

At 1280x800 the same curve reads 97,090 / 89,837 / 85,331 / 74,871 px with
the frame mean 89.63 -> 91.94 and the sky-mask IoU *improving* slightly
(0.4811 -> 0.4829). The tolerance is a **global** rasterizer depth bias
though: 0.02 NDC is ~1.3 m of blending at 14 m, so a wall in front of a
distant hillside lets the hillside bleed through. The right shape is a
**per-material band** (foliage wide, everything else tight) - a real shader
change with its own A/B.

### The albedo debug settles it: it is the WRONG SURFACE, not a darker one

At the masked pixels, `VF_SPLAT_DEBUG=6` (the palette albedo) reads **warm
bark/wood** while the SVO frame at the same pixels is **neutral grey**:

| pixel | splat rgb | SVO rgb | splat albedo (debug 6) |
|---|---|---|---|
| (543, 277) | 22 11 1 | 37 32 30 | 166 121 52 |
| (93, 18) | 39 20 6 | 79 61 48 | 177 129 56 |
| (120, 188) | 22 15 8 | 56 54 51 | 175 128 55 |

Mean saturation at the mask: **splat 0.828, SVO 0.273**.

So the splat path is painting a **different cell's** surface at those pixels -
a warm wood/bark cell rendered in shadow (hence near-black) where the SVO
renders a neutral cell. A wood/bark cell's disk has **spilled** over its
neighbour.

That reframes every shading hypothesis above as a **consequence**:

- turning the enclosure off (-33%) or the sun shadow off (-21%) does not
  stop the spill - it only makes the spilled disk bright enough to stop
  counting as near-black. The pixels are still wrong, just no longer black.
- relaxing the seal lets the *background* through (the true surface behind
  is depth-rejected), which is why the unsealed frame is brighter rather
  than more correct.
- the mask luma barely moves while the count falls (19.9 -> 20.7): the
  survivors are the spilled disks with the most extreme shading.

**So the one fix that would make those pixels RIGHT is to stop the spill -
and the clip, measured below, is not it.** The enclosure proxy, the binary
baked shadow and the bent normal are each worth fixing on their own merits -
the canopy really is being lit as a sealed room - but none of them is the
artifact either. What the measurements do support is narrower: the *shading*
of the disk that wins the pixel is ~half the gap, and the composite (which
disk wins) is the other half. No single lever closes both.

### The face clip, measured: a net loss, and why (the over-coverage is load-bearing)

The bake flags a disk that stands for exactly ONE exposed voxel face
(`mat_ao.x = material + 0.25`; 97,258 of 343,196 object surfels on the
hamlet) and guarantees the exact axis normal and the face-centre position.
The clip is then two lines in the fragment shader, and it must `discard` in
**every** pass, the depth-only prepass included - otherwise the prepass still
writes the spilled depth and the base seal still writes the spilled colour
at alpha 1, and the clip is a no-op.

Measured (tree at 640x400, the wall/house/water shots at 1280x720, before vs
after):

| shot | mask px | black-in-silhouette | sky % | px moved >12 |
|---|---|---|---|---|
| tree 640x400 | 21,856 -> 20,476 (-6 %) | - | 0.76 -> 1.14 | 18-30 % |
| cabin close-up | - | 30.30 -> **31.74** % | - | 18.0 % |
| house | - | 7.41 -> **8.72** % | - | 29.8 % |
| water | - | 3.76 -> **4.13** % | - | 21.6 % |

It removes 6 % of the artifact and pays for it with more black inside the
silhouette everywhere and more sky (the new holes let the sky through).

**Why: the over-coverage is load-bearing, and it is the EDGE, not the
corner.** The kernel is `alpha = 0.9 * exp(-0.5 * d2 / sigma2)` in
radius-normalised units, with `baseRadius = 1.4 * VOXEL = 0.14 m` and
`sigma2 = 0.5` (`surfelize.hpp:63`, `uSplat.y`). At a cell boundary the
pixel centre is 0.05 m from each face centre (`u = 0.357`), so each of the
two neighbouring disks contributes **alpha 0.79** there and the pair
composites to **0.957** - a ~4 % background bleed. That is today's seam.
Clip the squares and the same pixel belongs to exactly ONE square (they
tile, they do not overlap), so it is single-covered at **0.79** - a ~21 %
bleed along every 0.1 m cell boundary, on every surface, in every
direction. The corner is worse still (0.0707 m out, alpha 0.70), but the
corner is a point while the edge is a line: **the seam is the whole edge**.
So corner caps - the existing default-off emitter - cannot fix it; they
would cover the four-way junctions and leave every cell-to-cell edge open.

**What the clip does prove** is the shape of the real fix: a kernel that is
~0.95-1.0 *at the square boundary* tiles with no seam and no spill, because
a single square is then opaque on its own. That is a **kernel change** (a
flat-top / soft-square falloff, with all the softness inside the square),
not a clip, and it is the honest reading of the user's "better
interpolation": the Gaussian's overlap is what makes the surface solid, so
removing the overlap must be paid for with a flatter profile, not a smaller
radius. Its cost is that silhouettes become voxel-square at 0.1 m - which is
exactly what the SVO reference shows, but not what the splat path currently
looks like. It needs its own A/B and has not been built.

**Measurement trap, worth knowing before re-running this.** The clip was
reverted (both call sites are comments, the helper is gone,
`git diff shaders/` is empty) and rebuilt at 11:35:33. A headless render
loads `build/shaders/*.spv` at pass init, so a "clip" run started after that
rebuild measures the *reverted* build and reports a no-op: my own 1280x800
run differed from the pre-clip baseline by **16 px** (mean |d| 0.005) and I
first read it as "the clip is inert". The confirmation that Ada's pair is
clean is that the reverted build reproduces her `before/tree.ppm`
**bit-for-bit** (0 px changed > 2) while differing from her `after` by
52.6 % (mean 9.8, signed +1.56). Check the `.spv` mtime against the process
start before attributing a pixel diff to a shader change.

**What remains in the tree:** the bake flag stays (97,258 / 343,196 object
surfels, inert - no shader reads `fract()`), because a flat-top kernel
experiment needs exactly that per-disk face datum. The flag's other half,
the normal snap, is neutral on its own (see above) and is what keeps the
face squares well defined.


The ambient's direction was tried with the **geometric** normal instead of
the baked bent one (a foliage cell is one voxel thick, so the CPU
`aoBake`'s centre/bent taps are dominated by the cell's own leaf and the
baked bent points along the leaf - into the canopy - where the SVO's
per-pixel `sdfAO` at the exact face sees open sky). It barely moves: the
mask 21,784 -> 21,161 px at the default flags and 11,258 -> 10,965 px with
the enclosure and the sun shadow both off, and the luma at the mask does not
move at all (19.9 / 21.1). The baked bent is not the surviving dark term.
(The two backends' shading twins must stay textually identical, so the bent
stays anyway.)


The dark leaf is not "in front of" the lit surface. The disk is 1.4 cells
wide against a 0.1 m voxel face, so it spills ~4 cm past the face, and at a
canopy's density those spills **fill the gaps between the leaves**. The SVO
marches cells and its ray passes *between* the leaves and hits the lit
ground; the splat paints a dark leaf over that pixel. This reframing
explains every measurement at once:

- a smaller radius helps so much (less spill) - and a global shrink trades
  the silhouette away;
- unsealing "fixes" it by letting the **sky** through instead of the ground
  (the ground behind the leaf is depth-rejected), which is why the unsealed
  frame is brighter rather than more correct;
- the band tolerance only partly helps, because the lit ground is far behind
  the leaf's plane - it is not within any sane band.

So the principled fix is to **clip each disk to its own voxel face**, so a
disk cannot claim a pixel that belongs to another cell's face (or to a gap
between faces). The 1.4-cell radius stays for the Gaussian and the
cell-corner pinholes do not come back, because the seal fills what the clip
leaves. Two caveats: the clip is only well defined for a **single-face** cell
(the mean-normal multi-face cell has no single face to clip to), and an
**anisotropic** thin-structure disk spans several cells - both must be
excluded from the clip.


**The two terms that are real but secondary:**

1. **The enclosure test (bit 8), worth ~6 luma at the mask.** The splat
   backend decides "is this surface enclosed?" by marching the **coarse
   0.4 m object volume** (`skyVisibilitySplat` in `common_splat.glsl`), so
   a porous tree canopy always reads as a sealed room: its sky ambient is
   scaled to 0.16 and its IBL to 0.04. The SVO backend marches the *exact*
   geometry and says "open sky". A tree canopy is not a sealed room - that
   is a real bug, independent of the artifact.
2. **The baked sun shadow is binary, the SVO's is a 16-tap PCF.** The splat
   bakes one verdict per surfel (`shadowMarch` over the exact oracle, 0 or
   1); the SVO computes `softShadow` per *fragment*, so light reaches the
   canopy through the gaps between leaves. The splat path shades a
   back-facing leaf with no sun at all.

**What is NOT the cause (measured, and worth knowing):**

- **The footprint.** The disk radius is the right *lever* (`VF_SPLAT_RADIUS=0.5`
  swings the mask +31 luma while `VF_SPLAT_OPACITY=0.5` and
  `VF_SPLAT_SIGMA=0.25` are inert, so the error is spatial reach, not
  blending), but a *selective* shrink does not reach it: the shading-variance
  radius rule at FULL strength (`VF_CHAOS_SHRINK=1.0`) moves the mask
  21,785 -> 21,764 (0.1%). A global shrink is not the fix either: the
  silhouette IoU against the SVO is 0.4800 at the default radius, 0.4812 at
  0.7, 0.4837 at 0.85, but 0.4497 at 0.5 - it buys luma with 84k
  over-covered pixels traded for 74k holes.
- **The shading terms alone.** With the enclosure test and the sun shadow
  both off, half the masked pixels are still near-black (19.2 -> 25.6 ->
  36.0 against the reference's 62.9).


## Two populations, and what the seal cannot reach

Unsealing fixes most of the artifact but **not all of it**: at 1280x800 the
seal sweep leaves **24,372 px still dark (luma 21.9)** out of 94,116. So
there is a second cause the seal cannot reach.

**The "mean normal" half is falsified (2026-10-21).** The hypothesis was that
a multi-face cell's mean (45-degree) normal is where the residual lives, so
`VF_FACE_EXPAND` (one surfel per exposed face, exact axis normals) was built
in both the bake and the store path. Measured at 1280x800 with
`VF_CHAOS_SHRINK=0`:

| configuration | object surfels | mask px | frame mean | sky-IoU |
|---|---|---|---|---|
| default seal, expansion off | 343,196 | 94,116 | 89.66 | 0.4800 |
| default seal, expansion on | 515,834 (+50.3%) | 114,851 | 86.77 | 0.4662 |
| seal 1.0, expansion off | 343,196 | 24,372 | 102.17 | 0.4779 |
| seal 1.0, expansion on | 515,834 | 45,021 | 98.20 | 0.4690 |

Exact per-face normals make **both** populations worse: more disks means more
coverage, and the extra faces include back-facing ones that win more pixels
and shade dark. The change was reverted from `surfelize.cpp` (the file is back
at HEAD). The mean normal is **not** the residual's cause, and expansion is
not the structural half to build on.

**The depth ranking is also falsified (2026-10-21).** The candidate was to make
the depth **flat at the surfel centre** (`gl_Position = vec4(ndc * w, depC * w,
w)` in `splat.vert`), turning "nearest tilted plane" into "nearest centre"
(Voronoi ownership), with no `gl_FragDepth` write and the base `EQUAL` contract
intact. Measured at 1280x800:

| configuration | mask px | luma at old mask | frame mean | sky-IoU |
|---|---|---|---|---|
| default seal, plane depth | 94,116 | 18.65 | 89.66 | 0.4800 |
| default seal, flat centre | 97,090 | 19.51 | 89.63 | 0.4811 |
| seal 1.0, plane depth | 24,372 | 21.94 | 102.17 | 0.4779 |
| seal 1.0, flat centre | 18,002 | 35.39 | 107.43 | 0.4762 |

At the default seal the ranking metric makes **no difference** (slightly
worse), so the canopy's dark winner is already the nearest *centre*; under a
full unseal it improves the residual but makes the frame-brightening worse
(107.4 vs the SVO's 90.9). The brightening is therefore intrinsic to blending
the band without a seal, not an artefact of the plane depth. Reverted to the
plane depth.

**What that leaves.** The dark surfel wins the prepass, and the lit surface the
SVO sees behind it is rejected by the **band's depth tolerance** (0.002 NDC ≈
10 cm) before it can blend over - so the next measurement is a larger
`VF_SPLAT_DEPTH_TOL`, at the default seal. If a larger tolerance collapses the
mask without unsealing, the fix belongs to the band, not the seal.

## The other half is SHADING (2026-10-21)

Render-flag A/B at 1280x800 (plane depth, `VF_NO_OVERLAY=1`, baseline
94,116 px / 18.65 / frame 89.66):

| flags | mask px | frame mean |
|---|---|---|
| 511 default | 94,116 | 89.66 |
| 255 enclosure off | 66,994 (-29%) | 92.08 |
| 509 shadows off | 75,727 (-19.5%) | 102.27 |
| 253 both off | 53,894 (-43%) | 104.14 |

Ada measured the same shape independently at 640x400 (-33% / -21% / -48%),
so this is not a camera artefact. **Roughly half the mask is the shading
fields, not geometry**:

- `enclosed` is a ONE-RAY BINARY test (`skyVisibilitySPlat`) marched over the
  **coarse 0.4 m object volume**. That volume dilates a 1-cell (0.1 m) leaf
  into a ~0.4 m blob, so a dense canopy reads as a cave roof, while the SVO's
  `skyVisibilitySvo` marches the exact bricks and finds the gaps between
  leaves. With `enclosed ~ 1` the daylight ambient is scaled to 0.16 and the
  IBL to 0.04, so the canopy collapses to interior lighting.
- the same value takes `max(enclosed, aoShEnclosure(ao, sh))`, where the proxy
  is `(1-ao)*(1-sh)` - it fires precisely where both fields are dark, which is
  a canopy as much as a cave.
- the baked sun shadow is binary per cell at the cell's MEAN normal, so a
  multi-face leaf cell can be shadowed while the SVO's face-exact march is lit.

A **global** flag clear is not the fix: `VF_RENDER_FLAGS=255` also disables the
authored light sources and brightens every surface, and combining it with
`tol 0.02` costs the silhouette (sky-IoU 0.4639 vs 0.4800, +25,546 "sky" px
mostly from the brightened frame reclassifying). The targeted version is a
**baked per-surfel sky verdict evaluated on the exact `VoxelField`** (so the
splat agrees with the SVO's own march), keeping the terrain-cave start-column
test; a material-gated exemption for foliage is the cheaper approximation.
Both live in the bake (`src/voxel/surfelize.cpp`) and must be mirrored in the
live-store path, or a stamp re-aims untouched splats.

## The albedo evidence: it is the WRONG SURFACE, not the same surface shaded

Ada's `VF_SPLAT_DEBUG=6` (palette albedo) dump at masked pixels:

| pixel | splat rgb | svo rgb | splat albedo |
|---|---|---|---|
| (543,277) | 22,11,1 | 37,32,30 | 166,121,52 |
| (93,18) | 39,20,6 | 79,61,48 | 177,129,56 |
| (120,188) | 22,15,8 | 56,54,51 | 175,128,55 |

Mean saturation at the mask: splat 0.828, SVO 0.273. The splat's visible surfel
there is a warm **wood/thatch** cell, not foliage (mat 8 albedo is 10,132,8),
and the SVO's is neutral. So the splat is painting a *neighbouring cell's disk*
over a pixel that belongs to a different surface or to a gap - the 1.4-cell
disk spills ~0.04 m past its own 0.1 m face. That reframes the flag A/Bs above:
clearing the enclosure or the shadow does not stop the spill, it only makes the
spilled disk bright enough to stop counting as near-black; the seal dial lets
the background through instead of the true surface (which is depth-rejected).
Caveat: the comparison is splat ALBEDO against SVO FINAL COLOUR, and AgX
desaturates shadows, so the SVO numbers cannot prove which material it is - the
solid part is that the splat's albedo at the mask is not the material the user
sees as "the tree".

The **bent-normal** hypothesis was tested and is negative: using the geometric
normal for the sky term moved the mask 21,784 -> 21,161 px at flags 511 and
11,258 -> 10,965 at 253, with the luma at the mask unchanged (19.9 / 21.1). The
baked bent is not the surviving dark term, and the change was reverted.

## The face clip: the flag (implemented 2026-10-21)

The fix is to clip each disk to its own voxel face so it cannot claim a pixel
that belongs to another cell or to the gap between cells. The shader half is
`shaders/splat.frag`; the bake half is a per-surfel flag:

- `mat_ao.x = material + 0.25` for a **single-exposed-face** object cell.
  Decode `fract(vMat.x) > 0.1`. Every existing reader is blind to it:
  `uint(vMat.x + 0.5)` still yields the material (8.25 + 0.5 = 8.75 -> 8), and
  the `abs(vMat.x - 8.0) < 0.5` flora/shell tests still pass at 8.25 (checked
  in `splat.vert`, `splat_cull.comp`, `splat_tile_bin.comp`,
  `splat_tile_render.comp`, `splat.frag`).
- The bake also **skips the neighbourhood normal smoothing** for those cells,
  so the disk normal IS the exact axis and the disk position is the face
  centre (`cellCentre + axis*0.5*VOXEL`). That is what makes the face square
  derivable in the shader with no extra data: axis from the normal, the other
  two axes are the square's directions, half-extent `0.5 * pc.b.y`.
- Excluded: terrain (a heightfield sample is not a voxel face), multi-face
  cells (mean normal, no single face), thin footprints (an ellipsoid spanning
  several cells), and bridge/corner children (they clear the fraction).
- The store path computes the same flag from its own exposed-face mask
  (`SurfelCand::faceClip`), so a live edit cannot re-aim untouched splats.
- Coverage on the shipped hamlet: **97,258 of 343,196 object surfels** (28.3%),
  logged as `surfel face clip: N single-face object disks of M object surfels`.
- The snap alone (flag set, shader ignoring it) is visually neutral: mask
  94,116 -> 94,906 (+0.8%), frame 89.66 -> 89.74, sky-IoU 0.4800 -> 0.4809,
  13.6% of pixels moved by mean 0.96. So the clip A/B measures the clip.
- The shader must **discard** (every pass, including the depth prepass), not
  zero the alpha in the band: the base pass seals the nearest fragment at
  alpha 1 and the prepass writes its plane depth, so a band-only clip leaves
  the spilled colour and rejects the true surface behind it.
- Seam risk: at the face-square edge the Gaussian alpha is ~0.69 of centre
  (r 1.4 cells, sigma 0.5 normalized), so two faces meeting at a crease each
  reach the shared edge at ~0.7. Coplanar squares tile exactly and the seal
  covers equal-depth seams, but a wall close-up is the shot that would show a
  lattice grid; the canopy shot cannot see it.

**The regression to watch on that experiment is the WATER.** A constant
per-quad depth is systematically *nearer* than the true surface depth for a
tilted disk (by up to `rMax*sin(tilt)`). The water is a flat plane seen at
a grazing angle, so its plane depth IS the true water surface depth while
its cell's centre depth is nearer - switching to the centre depth pulls the
water toward the camera and lets it occlude the shore it should be behind.
The gate is the water shot in `tests/visual_check.py`. If it degrades, the
fallback is to keep the plane depth for the water surfels only (a one-line
branch on `surfelIsWater(s.mat_ao.w)` in `splat.vert`), not to abandon the
experiment.

**The face clip remains a candidate, but it is unproven.** It was proposed as
the companion to the expansion above; the expansion half is falsified and the
clip has not been measured at all:
1. **Clip each disk to its own voxel face** (the shader): a disk can then
   never claim a pixel that belongs to another cell's face, which is the
   over-coverage that lets a dark inner surface win. The 1.4-cell radius
   stays for the Gaussian, so the cell-corner pinholes do not come back -
   and where the clip leaves a gap, the *seal* is what fills it (that is
   precisely its documented job). Caveat: an anisotropic thin-structure disk
   spans several cells, so the clip must apply only to the isotropic
   face-sized disks, or it would destroy the thin footprints.

### Why the "coverage count in the stencil" does not work

The wiki's older idea - count the overlapping disks in the allocated D24_S8
stencil and decline the seal where more than one covers the pixel - **cannot
discriminate**. The disk radius is 1.4 cells *by design* (the Gaussian has to
fill the cell corners), so a surface's own neighbouring disks overlap
everywhere: **every terrain pixel is multiply covered**, and "decline the
seal where the count > 1" would unseal the whole world. It is also not
readable: Vulkan has no stencil read in the fragment shader, so the count
could only be used as a pipeline-level stencil test - which would reject the
base pass on every terrain pixel. Measured, not assumed: the count *is* a
monotone dial over the artifact, but the dial it turns is the wrong one.

### Instrument notes (both bite hard)

- `post.comp` applies **tonemapAgX to every pixel, including the debug
  views** - there is no bypass. So a value read out of a debug view is
  AgX-distorted and is not an absolute quantity (the normal encoding is not
  linear through AgX, so even its sign is suspect). Only *flat*
  classifications (`VF_SPLAT_DEBUG=16`'s ownership mask) and *final-frame*
  statistics are safe.
- `VF_SPLAT_DEBUG=5` is **not** albedo: the branch is `dbg > 5.5`, so 5.0
  falls through to the per-surfel texture heat ramp. Use **6**.
- The two IoU metrics **disagree in sign**: "object coverage against the SVO
  silhouette" improves when the seal relaxes (0.8955 -> 0.9626) because a
  washed-out surface still counts as covered, while the "sky-mask IoU" gets
  slightly worse (0.4800 -> 0.4779) because the relaxed seal leaks sky.
  Quote both, never one alone.

## The dead ends (each killed by a measurement, not by taste)


- **Coverage holes at the chunk boundary.** The user's six picks were flat
  sand tops straddling the `z=639/640` chunk boundary, each with exactly one
  supported splat (`r=0.14`, `n=(0,1,0)`); the store surface is flat across
  the boundary; `VF_SPLAT_DEBUG=1` (flat coverage) shows **zero** uncovered
  pixels at grazing views over that spot.
- **A stale height texture.** `VF_SPLAT_DEBUG=11` (heightfield residual) is
  uniform over the seam - redness 0.00 on every row of an 800x800 top-down
  frame centred on it.
- **The micro overlay.** Ruled out geometrically: the three micro layers sit
  at x = +11.55..+12.3, ~80 degrees off-axis at the locked camera, and the
  overlay only overwrites a pixel where a micro surface is *nearer*.
  (There is no `VF_MICRO` env switch in this tree - setting it renders the
  default twice, which is how it looked "ruled out" at first.)
- **Nondeterminism.** Two identical `--shot` runs differ by at most 5/255 on
  ~1.3% of the pixels, scattered - no line-shaped difference.

## The guard

`tests/visual_check.py` now renders both backends at the locked camera and
asserts the near-black mask stays at or below its documented baseline
(21,785 px, +15% margin for the per-run noise). It reports the count and the
mean luma at the mask. Tighten the baseline when the artifact is fixed;
never loosen it silently.

## See also

- [[concepts/enclosed-space-lighting]] - the bit 8 enclosure contract
- [[concepts/baked-sun-shadow-contract]] - the binary per-surfel shadow
- [[concepts/splat-edge-fade-measurement]] - how to measure a splat change
- [[concepts/headless-render-mouse-safety]] - render without stealing the mouse
