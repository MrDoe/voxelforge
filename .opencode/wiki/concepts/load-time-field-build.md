---
title: Load-time field build cost (why tests are slow)
tags: [performance, load-time, tests, voxel-field, startup]
sourceRefs: [src/voxel/voxel_field.cpp, src/app/main.cpp]
lastReviewed: 2026-10-08
---

# Load-time field build cost (why tests are slow)

Every `voxelforge` process pays a **~17.6 s world load** before its first
frame; `--shot`, `visual_check`, `live_edit_check` and the app all repeat it.
Measured breakdown on the hamlet world (16-core machine, 480×270 shot):

| phase | time | note |
|---|---|---|
| `VoxelField::build` (component EDT) | **13.2 s** | 72,002 components / 533k object cells |
| SVO synthesis | ~0.6 s | 4.6M records → 10.5k nodes / 36k bricks |
| `rebuildSurfels` | ~1.9 s | **4.19M** surfels — see the configuration note below before quoting this |
| splat upload + render + PPM | ~1.5 s | 4.19M surfels + the water grid |
| component grouping | 0.2 s | `groupComponents` |
| merge into hash/objVol | ~0 s | serial, but cheap |

So the field build is ~75 % of the load and the load is ~95 % of a test run.

> **Which surfel figure, and from where — RESOLVED (2026-10-08).** The figures
> below are **not three counts of the same set**, and the resolution was to
> **label** them rather than choose a winner:
>
> | figure | configuration | status |
> |---|---|---|
> | **3,398,081 / 2,168,454** | `VF_MICRO=1` / `VF_MICRO=0`; content set = Wiki's worktree at `HEAD a3144b6` (dirty `world_all.json`, untracked `make_vase.py`, `vase.vxw` present), binary md5 `e7b9c84f…`, explicit LOD env, hero cam 960×540 | **measured**; bake 3022 / 2346 ms |
> | **4.19M → 4.255M** | a **different configuration** (terrain 2.11M + object 0.79M + 64k edge + 774k LOD1 + 517k LOD2 = 4.255M with rounding); dirty tree, no recorded command | separately labelled, **not contradicted** |
> | 5.0M | superseded; object was 1.3M then | wrong |
>
> **`AGENTS.md`'s "3.40M → 2.13M" reproduces to 0.03 % and ~1.8 %** of the
> measured pair, so those figures **are** the `VF_MICRO` totals on a comparable
> content set. The earlier puzzle — 3.40M being *smaller* than base+LOD — is
> resolved by exactly the inference that flagged it: they were never the same
> set. **Micros cost +1,229,627 surfels (+56.7 %).**
>
> ### What the micro tail buys, and what that does not establish
>
> Repeat-run noise floor **0.085/255**; ON-vs-OFF frame delta **2.444/255**,
> **15.12 %** of pixels moving more than 4/255. So the micros are **visible at
> hero range**.
>
> That establishes **"visible"**, not **"worth 1.23M surfels."** The caveats are
> load-bearing: **single camera, splat backend only, no SVO arm, no HF metric.**
> Any cost/benefit claim beyond visibility needs a metric that exists on both
> backends.

### Micro removal landed — this table now reads as history

Micro removal is **implemented in the working tree (uncommitted)** — see
[[concepts/detail-pipeline]] §3. The restatement owed by the old pending note:

- `VF_MICRO=0` is no longer an A/B arm — it is **the default**. There is no
  `VF_MICRO=1` configuration in the app surface any more.
- The **+56.7 % / +1.23M** figure is **the cost that was removed**, kept here as
  the provenance for why the removal was safe — not as a live cost line.
- The `3,398,081` figure is the **with-micros** total: what the app baked before
  2026-10-08, not what it bakes now. The `2,168,454` figure is the effective
  current default bake.

So the field build is ~75 % of the load and the load is ~95 % of a test run.

## Why the EDT is bbox-bound, not content-bound

`VoxelField::build` groups object cells into connected components and runs a
padded-bbox signed-distance transform **per component** (~6 passes over the
*padded* box: occupied/argmat assigns, exterior BFS, two Dijkstra passes,
collect). The padding `kPad` (~8 cells) is load-bearing: the stored field
carries an air band (`kStore * VOXEL`), so it cannot simply shrink.

The problem is amortization: most components are single grass/pebble cells. The
padded extent is `content + 2·kPad` (`voxel_field.cpp:146-148`), so a **1-cell**
component spans `1 + 16 = 17` per axis = **4,913 cells**. Measured totals:

- components: 72,002
- object cells: 533,151
- padded bbox volume: **361,127,914 cells** (~680× inflation!)
- top-5 bboxes: 5.1M, 0.8M, 0.35M, 0.31M, 0.29M cells — the bulk is spread
  across the many mid-size components, not a few monsters.

**The measurements confirm the 1-cell picture rather than being consistent with
anything larger:** 361,127,914 / 72,002 = **5,014 padded cells per component
average**, i.e. ~17.1 cells per side. A 7-cell component would pad to
`23³ = 12,167` — **2.4× the measured average** — so the components cannot
predominantly be 7 cells.

> **Correction (2026-10-08).** This section previously said "their content is ~7
> cells while their padded bbox is `(2·kPad+2)³ ≈ 4900`". **Both halves were
> wrong, and neither was the measurement.** The measured inputs above were always
> correct and already implied a 1-cell average; only the derived arithmetic was
> off. `2·kPad+2 = 18` gives 5,832, which is not the 4,900 the same sentence
> quoted — so the expression and its own number did not agree. The code's formula
> is `content + 2·kPad`, and with `content = 1` it reproduces both the 4,900 and
> the measured 5,014 average.

The corrected reading is also the sharper one: **a single voxel is padded into
~4,900 cells of EDT work**, which is why the fix is batching rather than tuning.

361M padded cells × ~6 passes ≈ 2 G cell-ops ⇒ ~13 s. The Dijkstras are
already parallel over components (per-thread `Scratch`), so more threads do not
help; the work is simply oversized.

## Fix direction (next session)

**Batch spatially-near components into one processing group** (union bbox):
72k groups → a few k, amortizing the padding without touching the SDF math.
Keep the deterministic merge in component order (the collision rule is
"solid beats air-band, first-wins for solid-vs-solid"). Alternatives: a
narrow-band/sparse SDF pass (bigger rewrite), or a per-component bbox that
tightens to the stored band instead of a fixed `kPad`.

Do **not** shrink `kPad` (it carries the stored SDF air band) and do not
parallelize renders instead — the load already saturates the cores (3
concurrent `--shot` runs took 48 s vs 53 s sequential).

## Test-time consequences

- Trimming render counts helps only linearly (each render is ~18 s, mostly
  this load); parallel renders do not help (see above).
- `ctest -j4` is the only wall-time lever until the load shrinks (the 4 test
  binaries are independent). `live_edit_check.py` is ~6 min for ~20 renders,
  `visual_check.py` ~1 min for 3.
- The render-level checks cannot be replaced by the (fast) unit tests for the
  GPU-patch paths — see [[entities/live-edit-brush]] for the zero-surfel patch
  regression that no store-level test could see.
