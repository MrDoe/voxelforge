---
title: Surfel holes on walls and stepped roofs (buried-cell fill)
tags: [surfel, splat, normals, sdf, roof, walls, bug, fixed]
sourceRefs: [src/voxel/surfelize.cpp, src/voxel/voxel_field.cpp, src/voxel/voxel_field.hpp, shaders/splat.frag]
lastReviewed: 2026-09-19
---

# Surfel holes on walls and stepped roofs

**Symptom (reported 2026-09-19).** Walls and the stepped roof of the hall
read as "unsolid": dense dark horizontal slots / speckle, strongest at
grazing angles (e.g. looking up at the roof from below the eave, or along a
wall). SVO (`--mode svo`) renders the same view clean, so it is a
splat-backend artifact. Measured on the south-wall view (`cs10` camera):
4.35 % of the frame darker than the local median vs 0.83 % in SVO.

**Root cause: buried cells emit +Y-fallback disks.** The object field marks
*enclosed* air (building interiors, the hollow roof/wall shells, sealed
cavities) as **solid** — `solid = occupied || !exterior` in
`VoxelField`'s component distance transform (`voxel_field.cpp`), by design
(the model treats a component as a filled volume; the SVO bakes it solid
too). `buildSurfels` enumerated every cell with `d <= 0`, including cells
whose whole 6-neighbourhood is solid-or-enclosed. `meanNormal()` had no air
neighbour there and returned the `(0,1,0)` fallback, so those invisible
buried cells emitted **up-facing disks** that:

1. **polluted the normal smoothing** of the real surface cells (the wall's
   front disks came out tilted ~11° up; the roof risers banded), and
2. **filled the enclosed volumes** with dark disks (AO 0.15-0.21, shadow 0)
   that are the nearest surface at grazing angles — the depth-resolve band
   (see [[concepts/detail-pipeline]]) then blended them through, and the
   pixels read as dark holes/slots.

A third contributor (gone with the micro removal 2026-10-08 — see
[[concepts/detail-pipeline]] §3): the micro-detail disks' facet normals (tilt 0.35-0.9
rad) made each 2-6 cm child shade visibly darker/lighter than its base cell
at mid distance, adding speckle on flat planks and the roof. Recorded as
history: the diagnosis was correct for the frame it was made on, and the
contributor no longer exists to verify against.

**Fix (surfelize.cpp).**

- `meanNormal()` now returns a **zero** vector when no neighbour is air.
- After pass 1, `buildSurfels` **drops** those cells (`keys`/`rawNormals`
  filtered before pass 2, so they also leave the smoothing and shadow/AO
  neighbour averages). The live/store path already did this
  (`collectChunkCandidates`: `if (!surface) continue`), so the bake now
  matches it.
- Micro-detail facet contrast for non-foliage materials halved (tilt,
  `aoMul` -> 0.94-0.98); foliage (mat 8) keeps its high tilt — there the
  facet noise *is* the canopy volume.
- The original 2026-09-19 edge experiment removed object growth but then
  shrank from normal disagreement, which was still too broad and could create
  fresh pinholes. Superseded on 2026-09-25 by [[concepts/edge-aware-surfel-radius]]:
  normal disagreement never reduces object coverage; only occupancy-proven
  hard-edge parents tighten, and small always-on crease bridges repair the
  centre line. Terrain keeps disagreement growth for heightfield sealing.

**Verification.** `vf_tests` 76/76; `visual_check`, `live_edit_check`,
`texture_check` pass (`ssao_check` still fails only the pre-existing
marginal `hero/svo` AO threshold — the SVO path does not use the
surfelizer). `cs10`: dark 4.35 -> 3.12 %, high-frequency energy
5.98 -> 5.12; hero: 5.08 -> 4.61 % / 5.22 -> 4.55.

Cross-refs: [[concepts/detail-pipeline]] (the four detail layers and the
micro path), [[entities/svo-render]] (the reference backend the artifact was
compared against), [[concepts/load-time-field-build]] (the SDF/enclosed-air
model).
