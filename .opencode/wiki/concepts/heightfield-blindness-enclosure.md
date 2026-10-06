---
title: Heightfield Blindness — Two Splat-Path No-Ops Fixed by Reading the Column Once
tags: [splat-backend, enclosure, sky-visibility, point-lights, carve, cave, heightfield, gotcha]
sourceRefs:
  - shaders/common_splat.glsl
  - shaders/common_base.glsl
  - shaders/common_svo.glsl
  - .opencode/wiki/concepts/enclosed-space-lighting.md
lastReviewed: 2026-10-06
---

# Heightfield Blindness

A pattern, not a single bug: the splat backend's occlusion tests had **two**
independent blind spots, both invisible because each returned "no-op" rather
than an error. Both are now fixed. Test scene: a carved hillside cave, 4 m
diameter, 6 m deep, voxel 192,589,752, isolated `VF_OVERLAY_PATH`.

The shared lesson: **the object volume is blind to terrain that was carved
away, and the heightfield is blind to overhangs.** Whichever one a test leans
on, there is a class of geometry it cannot see — and a test that returns
"nothing occluded" looks identical to a correct test on open ground.

## (a) Enclosure: the `aoShEnclosure` term was pinned to zero

`aoShEnclosure(ao, sh)` isolates "no direct light AND no local sky exposure"
by multiplying `(1-ao)(1-sh)`. For a terrain cave it was **always 0**, for two
compounding reasons:

1. The object volume cannot see a cave carved out of *terrain* — there is no
   object there at all.
2. The bake skips the shadow march on backfacing surfels (`sh = 1` when
   `dot(n, kSunDir) <= 0.02`), which is most cave walls. So the one field that
   *did* know about the carve was forced to 1 exactly where it was needed.

**Measured:** before, 45.26 mean luma enclosure-ON vs 45.84 OFF — a no-op.
After adding a heightfield term to `skyVisibilitySPlat`: **36.51 ON vs 45.84
OFF**, with the OFF path byte-identical, so `VF_RENDER_FLAGS=255` is now
honest rather than approximately honest.

### The fix, and why the threshold is 0.35

```glsl
// Only the START column is tested — a heightfield has no overhangs, so
// terrain can never hide a ray that has already left the surface.
if (heightAt(p.xz) - p.y > 0.35)
    return 0.05;
```

Only `p` is tested, not the march, because terrain occluding a ray that has
already exited the surface is impossible for a heightfield. The 0.35 m
clearance absorbs half-voxel quantisation plus the surfel's `n*(0.5*VOXEL)`
offset on a slope — a real cave roof is metres above. Testing `p` alone also
keeps an outdoor point (`p.y == heightAt`) from being called buried.

`shadeSurfel` additionally had to switch to the **RAW** baked shadow
(`shRaw`) rather than the caller's gated `sh` — the gate fires on almost every
cave wall and was silently zeroing the product.

## (b) Per-light march: a lamp inside its own cave lit nothing

`lightVisibilitySPlat` marches the heightfield. Underground, `sHf` is a large
negative at *every* tap, so the first tap returned 0 and the light never
reached the room it was in.

**Measured:** an interior lamp moved the splat frame by **0.00** mean luma
while the SVO backend, which traverses the real carved geometry, moved it by
0.51. The lamp was in the scene and did nothing.

### The fix: skip the terrain tap when BOTH endpoints are buried

```glsl
const bool buriedBoth = (heightAt(ro.xz) - ro.y   > 0.05) &&
                        (heightAt(end.xz) - end.y > 0.05);
```

Sound because **a heightfield has no overhangs: a segment that never crosses
the surface cannot be crossing terrain.** The step size also switches to the
object field alone while buried — otherwise the large negative `sHf` collapses
the march step to ~a third of a voxel and the march never arrives. The moment
either end rises above ground the normal test resumes, so a lamp behind a hill
still does not light the far side.

**Measured after fix**, lamp at a verified in-cavity point:

| | mean luma | warm% |
|---|---|---|
| splat | 36.51 → **54.12** | 0 → 17.1 |
| SVO | 48.58 → **100.85** | 0 → 77.7 |

An out-of-range lamp changes nothing in either backend, so the guard did not
simply leak light everywhere.

## Open divergence

Magnitudes still differ: splat +17.6 vs SVO +52.3 mean luma for the same lamp.
The sign and direction agree — the no-op is fixed — but the absolute response
is not parity. Not recorded as solved.

## Generalisation

When adding an occlusion or enclosure test in the splat backend, ask which
field is answering and what it cannot see:

| Test | Object volume | Heightfield |
|---|---|---|
| sees a room carved from terrain | **no** | yes (as "solid everywhere") |
| sees an overhang | yes | **no** |
| sees a lamp behind a hill | partly | yes |

Each was individually defensible and jointly wrong. The bug class is a test
whose *blind case* returns the same value as its *pass* case.

Related: [[concepts/enclosed-space-lighting]] (the bit-8 enclosure lane and
the light UBO), [[concepts/sun-direction-pipeline]] (`kSunDir` consumers).