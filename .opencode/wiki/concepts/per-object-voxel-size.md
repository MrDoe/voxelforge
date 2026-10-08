---
title: Per-object voxel size — GPU perspective analysis (2026-10-08)
tags: [voxel, format, bricks, surfels, design]
sourceRefs: [AGENTS.md, src/voxel/worldfile.hpp, src/voxel/chunk_store.cpp, src/voxel/editable_world.cpp, src/voxel/layered_world.cpp, shaders/common_base.glsl]
lastReviewed: 2026-10-08
---

# Per-object voxel size — a GPU-perspective analysis

Question raised 2026-10-08: can an object be voxelized at a *finer* voxel
size than the global 10 cm? Answer: yes, but the interesting cost is not
where intuition places it. The GPU never sees voxels.

## What the GPU consumes

- **Splat backend (primary):** surfels — world-space disks carrying
  `pos/normal/radius/material/tangent`. Resolution-blind: a finer lattice
  downstream just means more, smaller surfels in the same chunks. Cost:
  instance count (~4× quads for a 2× finer lattice on the same surface),
  tile-binner atomic contention, and depth-band overdraw. Shading math
  unchanged.
- **SVO backend (reference):** chunk-local pools + bricks. The brick byte
  format is where the lattice is load-bearing: CPU `sdfRaw = int(d / VOXEL)`
  truncated, shader decodes `raw * VOXEL` from a compile-time constant, and
  the DDA tests `sdf <= 0` as solid. A fine brick halves the metric reach of
  one SDF byte (±127 × 0.05 m = 6.35 m vs 12.7 m at 0.1 m). Supporting
  per-object size there means per-brick voxel-size metadata and separate
  pool formats — the expensive part.

## Where "10 cm" actually lives (all CPU-side)

- `VoxelRecord` stores `uint16_t x,y,z` grid indices; the interpretation
  `p = -worldSize/2 + idx*voxelSize` comes from the single
  `WorldFileMeta.voxelSize` in the VXW header. `EditableWorld::load` and
  the bake reject meta mismatches — a fine object written with the global
  meta would be *silently misplaced*, not flagged, at merge time.
- `VoxelField` merges layers first-wins-a-cell (`layered_world`) — assumes
  one shared cell per position.
- Chunk indexing (`chunkIndexOfCell`), the live-edit overlay schema,
  `Chunk::lo/hi` lattice bounds math, and the normal pipeline all key on
  the global `VOXEL`.

## What it would take

A true per-object voxel size is a **format-ownership change**, not a
shader change:

1. VXW v2 sections already support per-section payloads — carry a
   per-layer meta (world size, voxel size, grid N) alongside each
   records section.
2. `VoxelField` becomes per-layer (per-component `CompOut` stays keyed to
   its layer's lattice), instead of merged at record level.
3. Merge moves from records to **surfels**: each layer's store path emits
   surfels into the same world-space chunks the GPU already bins. The
   splat path needs no shader change for this.
4. The SVO reference path needs per-layer brick pools and a decode
   constant update — the SVO is the main blocker for full parity.

## The GPU perspective (rendering-side)

Fledge's summary above is the **representation-side** answer: CPU format work,
splat path nearly free, SVO brick format the blocker. This section is the
**rendering-side** answer, and the two are complementary — both must hold for a
finer-lattice object to work.

**The splat rasterizer is indeed nearly free FOR INSTANCE COUNT.** The shader does
not care about absolute disk size; a 1 cm disk and a 10 cm disk rasterize
through the same code path. Mixed-scale surfels in one pass are feasible. **But
not for coverage:** a fine object is made of smaller disks, and smaller disks
need MORE overlap per pixel to reach the seal alpha. Push too fine and you
land in the documented thin-structure failure — a sparse field of splats that
does not read as a solid. "Free" is right for the surfel count and wrong for
coverage. Note the thin-structure rule (long lattice run + ≤3-cell
cross-section → narrow ellipse) is this renderer ADMITTING the lattice is the
limit and faking sub-voxel features with elongated disks — the strongest
existing evidence that genuine sub-10 cm geometry is wanted, and the reason
the fine-proxy path is credible rather than a compromise (children already
inherit parent shadow/AO, and a fine proxy would inherit the coarse cell's the
same way).

**The blocker is coverage, not rasterization.** A 10 cm terrain surfel is a
10 cm *disk*. A 1 cm vase wall in front of it is made of 1 cm disks. The
terrain disk extends well beyond the vase's coverage, so it **pokes through** —
coarse terrain shows through a fine object. Fixing that means splitting coarse
surfels where a fine object intersects them: a CPU cost, a complexity cost, and
a break of the single-merged-record-set invariant that `VoxelField`, SVO,
surfelize, overlay and picking all rely on.

**The SVO is the harder blocker, and it is a format blocker.** Bricks are
fixed 8³ cells at 0.1 m. A finer object needs finer bricks, which changes the
brick *format* — not just the decode constant. That is deeper than the splat
path and it is the reference backend, so any divergence is a correctness
problem.

**The bake is a third, independent blocker.** The EDT is already bbox-bound at
~13 s. A 1 cm object has a bbox-to-content ratio far worse than 10 cm terrain —
the padding inflation gets *more* severe, not less. And the component count
can INCREASE with resolution: a one-cell bridge at 0.1 m can vanish at
0.025 m and split a component — and components drive the padded-bbox EDT — so
finer detail can make LOADING slower, the opposite of what intuition says.

**Mesh import regresses relatively as you go finer** (the trap worth
remembering): the 13-axis SAT voxelizer inflates the solid by up to ONE
voxel per axis. That is 0.1 m at the current lattice but up to 25 % of a
0.1 m rail at 0.025 m. Those inflated cells are REAL solid cells, so each
inflation band cell is also an extra exposed face → extra surfel → extra
shadowMarch sample. The practical rule: assert object bounds with a one-voxel
tolerance **in metres** against the source mesh (never cell-count equality,
which looks like a voxelisation bug and is actually the documented
padding), and key placement on the SOLID cell AABB, not the padded grid
origin.

### The one GPU answer that works: render the fine object as triangles

The mesh is already there (STL/OBJ). A standard rasterizer handles arbitrary
resolution with **no coverage problem** (triangles have no disk that can poke
through), **no second octree**, and **no EDT**. Composite with the voxel world
through the shared depth buffer — the depth test handles occlusion naturally in
both directions.

The costs are real but different: the mesh needs the same PBR shading model as
the voxels (a second shading path), and its shadows need to be consistent with
the voxel world's baked shadows. Those are *shading* problems, not *geometry*
problems, and the shading infrastructure already exists.

### Verdict

Per-object finer voxels are **possible but expensive**, and the cost is
concentrated in three independent places: representation (SVO brick format),
rendering (coverage), and the bake (EDT inflation). The mesh-as-triangles
approach sidesteps all three GPU costs at the price of a second shading path.
The detail layers sidestep all of it at the price of no true sub-10 cm
geometry.

**For a hero prop where the silhouette matters, mesh-as-triangles is the
better trade. For everything else, the detail layers are the better trade. The
lattice stays at 10 cm.**

### Bridge to the landed implementation — and a correction

An earlier draft of this section proposed to bridge to the normalizing
layer-load by saying it "sidesteps the disk-pokes-through issue entirely,
because the fine object's records become coarse records before they reach the
surfelizer." **That was wrong**, and it was wrong in the direction that
overstates: it assumed the normalizing load preserves fine geometry. It does
not. Wiki's landed `resampleRecords` resamples *into* the world lattice, so it
sidesteps the coverage problem by **giving up the fine geometry**, not by solving
it. The page's own status line is the accurate one: authoring convenience and
coverage, still no sub-10 cm geometry.

The correction matters because the two sections read as complementary when they
are actually in tension:

| | preserves fine geometry? | coverage | what it buys |
|---|---|---|---|
| `resampleRecords` (landed) | **no** — resampled into the world lattice | clean (one lattice) | authoring convenience, correct scaling |
| fine-raster surfel stream (**not built**) | yes | needs the fine stream to occlude correctly | actual sub-10 cm geometry |

And the cost analysis is *not* where intuition puts it. Wiki measured the surfel
bill for the finer vase at **~0.27 MB — noise against the ~3.4 M-surfel scene**.
The count is not the problem. So the case against a fine-raster stream is
**correctness, not budget**: the fine stream must occlude the coarse one
correctly, and the SVO/voxel-fed marches (shadow, water DDA, irradiance) still
see no detail. That is why the unbuilt design keeps a **coarse field for
shadow/picking** alongside a **fine surfel stream** — two representations, one
visual.

## The practical alternative

The engine already expresses an object's detail budget as *splat density
and normals*, not voxel count: crease bridges
(`VF_EDGE_FILL`), anisotropic footprints (`VF_ANISO`), texture detail
normals (bit 7). (Micro-surfels were on this list until their removal
2026-10-08 — see [[concepts/detail-pipeline]] §3.) Most of the visible win of "finer voxels on one object"
is reachable today without splitting the lattice.

## Empirical anchor (2026-10-08)

Resampled the real `assets/vase.vxw` from 10 cm to 2.5 cm cells (pure
subdivide of the record solid — an optimistic lower bound for a true
source-mesh bake):

| | cells | exposed faces | ~parent surfels* | surfel bytes @80B | record payload @16B |
|---|---|---|---|---|---|
| 10 cm (authored) | 869 | 1,046 | ~209 | ~17 KB | 13.9 KB |
| 2.5 cm | 55,616 | 16,736 (**16.0×**) | ~3,347 | ~267 KB | 889 KB |

*exposed-face count ÷ ~5 as a crude parent-disk proxy.

So the surfel bill for the finer vase is ~0.27 MB — noise against the
~3.4 M-surfel scene. The count is NOT the problem. What you cannot buy
at 16× is: SVO/voxel-fed marches (shadow, water DDA, irradiance) see no
detail, and the padded-bbox EDT can grow with component splitting. The
practical implementation therefore keeps fine cells OUT of the coarse
oracle and feeds the splat path directly.

**Implementation exists — COMMITTED, NOT MERGED.** Commit **`a3144b6`** on
branch `feature/per-object-scale`, worktree
`/home/christoph/code/voxelforge-per-object-scale`. Not on `master`; nothing
below describes `master` behaviour.

*Source of this section: reported by the session that built it, with the
committed hash and worktree path attached.*

**What landed.** `worldfile::resampleRecords()` + a per-layer `"scale"` manifest
field. Object/scatter layers whose `voxelSize`/`gridN` differs from the world
lattice are resampled into it with **overlap-weighted dominant-material voting
about the bottom-center pivot**. `scale` is a unitless ratio, default 1, written
only when `≠ 1`. `LayeredWorld`'s cache re-normalizes on a scale change and its
dirty hash folds `scale` in; `EditableWorld::importLayer` resamples mismatched
sources. **Non-object roles still reject** a meta mismatch.

**Verified at that commit** (clean tree, `test-world` green, `--selftest` green,
new doctest cases):

| case | result |
|---|---|
| 10×10×10 block authored on a 5 cm grid | exactly 5×5×5 cells |
| `scale: 0.5` on a matching-meta block | ≤ 3 cells wide |
| manifest round-trip | passes |
| landscape-role meta mismatch | still rejected |

**Verified render measurement** — config: splat default, 640×360, camera
`-3.0 1.7 16.8 → 0.1 0.75 19.95`, `VF_NO_OVERLAY=1`, worktree assets. At
`scale: 0.5` the vase's visible footprint is **0.42×** the authored area
(2667 vs 6292 changed px vs vase-off, threshold 10/255), against a
**194-px same-config-twice noise floor** — so the signal is ~14× the noise.

**Vase fine-resample arithmetic.** 869 → **55,616** cells; 1046 → **16,736**
exposed faces (**16.0×**) at 2.5 cm; ~**0.27 MB** of surfels at 80 B. This is the
number that settles the cost question: the surfel bill is noise against a
~3.4 M-surfel scene, so **count is not the obstacle** — correctness is.

> **The caveat travels with every figure above:** this path **trades fine
> geometry away**, it does not deliver sub-10 cm geometry. `resampleRecords`
> resamples *into* the world lattice, so it sidesteps the coverage problem by
> discarding detail rather than solving it.

### Pending merge — this is the state that goes stale quietly

`a3144b6` is **committed on a branch, not merged**. Every other page therefore
still reads correctly *today*, and the drift only becomes visible after the
fact — by which point the pages have moved and nobody is looking.

### Status 2026-10-08 — Phase 1 reported green on its own cases (reported, not wiki-verified)

Victor reports: `importLayer` hunk re-applied, `vf_tests` builds, **4 worldfile
cases green (429 assertions incl. fine-grid resample)**. One remaining failure,
`test_world.cpp:613 slicesWithSky` (4096 vs 64), is attributed to Vega's dirty
test plus untracked irradiance files — **not** the merge path, which is
byte-identical at scale 1. Confirmation of that attribution is routed to Vega;
until he confirms, treat it as reported rather than settled.

Phase 1 boundary holds: **representability only, no fine geometry claimed.**
Nothing on this page moves on the strength of this report — the `scale`
fifth-field note and the three-behaviours doc debt stay pending until merge.

### Settlement part one 2026-10-08 — `importLayer` landed in worktree, uncommitted (reported)

Victor reports, via coordinator call gated on Fledge's 15/15 green: **5 port
files + `importLayer` hunk accepted in the worktree, uncommitted.** Filed here
as a landing state, not a merge state — the pending-merge note above still
holds, and every figure on this page is still a branch figure.

**Still open:** `slicesWithSky` attribution with Vega (reported-not-settled).
Texture-header recurrence: re-implemented by Fledge from the usage sites,
filed as instance two on [[concepts/uncommitted-edit-is-not-yours]]; durable
committed gate still open on Victor's claim.

**Known doc debt at merge:** `a3144b6` changes **three documented behaviours**;
`AGENTS.md` wording is owed at merge and is owned by the coordinator, not by the
branch author. The specific three are requested from the branch author so they
can be annotated here rather than discovered by a blind sweep.

**On merge, re-check rather than re-trust:** the `"scale"` field's interaction
with placement (applied about the **bottom-center pivot**, with `LayeredWorld`'s
dirty hash folding `scale` in) is currently sourced from the author's
description, not read back off the commit.

A true fine-raster surfel stream (fine cells → world-space surfels, coarse field
kept for shadow/picking) is the design the analysis above points at; it **has
not been built**. See [[concepts/improvement-roadmap]] for the phase split now
under way — **Phase 1 is representability only and claims no fine geometry.**

Cross-links: [[concepts/improvement-roadmap]],
[[concepts/live-edit-surfel-parity]],
[[concepts/detail-pipeline]], [[concepts/edge-aware-surfel-radius]].
