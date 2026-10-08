---
title: "Dynamic sun shadows: re-bake from the resident field, do not move the march to the GPU"
tags: [shadows, sun, surfel, splat, shaders, performance, gpu, sun-direction]
sourceRefs: [src/app/frame/run.cpp, src/app/frame/run_poll.cpp, src/app/world/surfel_stream.cpp, src/app/world/world_layers.cpp, src/voxel/surfelize.cpp, shaders/common_splat.glsl, shaders/common_base.glsl, shaders/splat.frag, .opencode/wiki/concepts/sun-direction-pipeline.md, .opencode/wiki/concepts/load-time-field-build.md]
lastReviewed: 2026-10-08
---

# The sun moves, and the splat path stalls ~13 s doing work the sun did not change

> ### ⚠ DECISION half-landed 2026-10-08 — emission live, shadow-map lane open
>
> User order 2026-10-08 (via George, topic `dynamic-lighting`): sun + lights
> **re-derive live** — no baked shadow, no stall, no 16-cap. Configuration as
> landed: **budget 192/K=4, unbounded-adjustable**. This supersedes the
> static/thinned chain documented across
> [[concepts/baked-sun-shadow-contract]] (baked per-surfel shadow) and
> [[concepts/enclosed-space-lighting]] (1.5 m thinning, 16-cap).
>
> **Emission path LANDED (Wiki):** store enumeration (169 clusters incl.
> submerged lava), authored-first shared fill, stroke-end trigger + preview
> overlay + follow-attachment (`"follow"` lamps ride Move/Rotate). Parity
> green (7/7), store group 2/2; 169-light night holds ~85 fps (11.68 ms avg,
> 960×540) under the per-pixel K=4 march cap. (House night ground +32% is
> Victor's authored-lamp retune, same day — complementary, not flip-caused.)
> **Open (Victor):** the
> shadow-map lane that removes the sun re-bake analyzed below — the sun
> still routes through `requestWorldReload()`, so everything below describes
> the tree as it is until that lane lands.
>
> **Visibility proof required:** the user reports **zero visible change** so far.
> A lighting change with no visible effect is indistinguishable from a dead
> instrument — see [[concepts/measurement-discipline]] on clean zeros. Landing
> needs a measured frame delta above the noise floor on at least one canonical
> view, not just green gates.

The splat backend bakes its sun shadow **per surfel on the CPU**. So the sun
cannot move without a re-bake, and `setSunAngles` reaches that re-bake through
`requestWorldReload()`, which is a much larger hammer than the job needs.

This page records the two findings that shape the fix, both established **from
source**. The measured A/B is still open — see [Measured, pending](#measured-pending).

## Finding 1: a sun change re-does the per-component EDT, which has no sun in it

The call chain, in order:

| step | site |
|---|---|
| `setSunAngles(elev, azim)` | `src/app/frame/run.cpp` |
| `requestWorldReload()` → `m_pendingWorldReload = true` | `src/app/app.hpp:613` |
| `m_layers.requestReload(m_camera.pos, true)` | `src/app/frame/run_poll.cpp:35-37` |
| full `LayeredWorld` reload, incl. `VoxelField::build` | `src/voxel/layered_world.*` |
| `applyWorldReload()` → `rebuildSurfels()` | `src/app/world/world_layers.cpp:71` |

Per [[concepts/load-time-field-build]], `VoxelField::build` is the ~13 s of a
~17.6 s load — a padded-bbox two-pass Dijkstra EDT per object component (72k
components, 361M padded bbox cells). **None of it depends on the sun.** The
field is geometry.

Everything that *does* depend on the sun is three lines deep in
`rebuildSurfels()`:

```cpp
sp.sunDir = glm::vec3(m_sunDir);                              // surfel_stream.cpp:333
...
vf::voxel::SurfelSet set = vf::voxel::buildSurfels(m_layers.field(), sp);  // :359
```

`shadowMarch(store, pos + cd.n * 0.3f, sunDir)` in `shadeCandidates`
(`surfelize.cpp:2038-2046`) is the only consumer. And line 359 is the whole
reason this is cheap to fix: **the re-bake already consumes the resident
field.** It never asked for a rebuild.

So routing a sun change straight to `rebuildSurfels()` yields a moving sun with
no field rebuild. What remains is surfel enumeration + shading + a full
surfels-buffer re-upload — real work over ~2.13M surfels (post-micro-removal
default bake), but categorically different from 13 s of EDT.

**Do not call this instant.** The remaining re-bake is unmeasured; see below.

### Two caveats for whoever lands it

1. **The live-edit overlay would not be re-applied.** `applyWorldReload()` is
   what calls `loadStoreOverlay()`, so calling `rebuildSurfels()` alone skips
   it and chunks edited in-session keep their **stale** baked shadows. It needs
   an overlay re-apply or a per-chunk re-seed — the same obligation every
   bake-then-patch path carries (the removed `U` micro-detail toggle used to
   be the reference example).
2. **This is a splat-only fix** — *verified in source*. The SVO reference
   backend marches its shadow per fragment: `softShadow` (`common_svo.glsl:396`)
   is built on `exactSVOHit` (`:346`, the exact binary DDA), and `shadeTerrain`
   calls it per fragment at `common_svo.glsl:583` —
   `((gRenderFlags & 2) != 0) ? softShadow(p + n * 0.35, kSunDir) : 1.0`.
   Only the splat path bakes, so only the splat path stalls.

## Finding 2: a per-fragment march would be both slower and less accurate

The obvious move — reuse `softShadowSplat` for every opaque surfel instead of
the baked value — loses on both axes.

**Accuracy.** The bake marches the **exact 10 cm lattice**
(`shadowMarch` over `VoxelField::sample`). `softShadowSplat` marches the
heightfield plus `objDist`, and `objDist` is the coarse `r8_snorm` 256³ volume:
0.4 m texels across the 102.4 m world, clamped to ±1.26 m, with empty reading
exactly `+kObjVolMax`. So per-fragment object shadows would be roughly **4×
coarser** than today's, and terrain shadows would read a *smoothed bilinear*
heightfield that cannot represent overhangs. It would also drift from the SVO
reference, which is precisely what the reference exists to catch.

**Cost.** `softShadowSplat` is 16 iterations of up to two dependent texture
fetches (`heightAt` + `objDist`) — up to **32 dependent taps**. And the splat
pipeline shades the front surface **twice** per covered pixel, verified in
source rather than assumed: `drawOpaque()` in `splat_pass.cpp` issues the same
instanced draws twice over one fragment shader — first `m_opaqueBasePipe`
(depth-compare `EQUAL`, opaque replace), then `m_opaquePipe` under a
`vkCmdSetDepthBias` (`splat_pass.cpp:2279-2334`). `shadeSurfel` is called once,
at `splat.frag:324`, and both pipelines run it. That is up to ~64 dependent
taps per covered pixel, in a pass that `AGENTS.md` already measures at ~57 ms
forward geometry at 720p hero.

The bake is therefore **not a workaround for a missing GPU march** — it is the
higher-quality path, and it is cheap only because it runs once per surfel
instead of once per fragment.

### If a per-fragment shadow is ever genuinely wanted

A **sun-space depth map**, re-rendered when the sun changes. The projection
math already exists in the compute cull pass (`splat_cull.comp`), so the sun
pass is rasterize-surfels-from-the-sun-POV into a depth target, then 1–4 PCF
taps per fragment instead of 32–64. Cost moves to the sun-change event, which
is where the current architecture already puts it.

## The shader A/B harness (no shared files, baseline proven)

Two facts make an isolated variant cheap here:

- `CMakeLists.txt:164-195` compiles with
  `glslangValidator -V --target-env vulkan1.3 -I<src>/shaders -o <out>.spv`.
  Because `-I` is a *path*, a **clone** of `shaders/` compiles standalone.
- Every pass loads `VOXELFORGE_SHADER_DIR` at init (`splat_pass.cpp:319,447`,
  `svo_pass.cpp:121`, and the ssao/ssr/fog/taa/post/dof/env passes), so
  `VOXELFORGE_SHADER_DIR=<scratch>` renders a variant **without editing the
  tree**.

Verified on 2026-10-08: a scratch clone compiled from source md5 `2d3202cd42bc32dff4c1b6cf7a67a9f3`,
**byte-identical** to `build/shaders/splat.frag.spv`. So the harness baseline is
*proven*, not assumed, and any measured delta is attributable to the variant
rather than to path or nondeterminism.

### Two traps in probing the shader tree

- **The four `common_*.glsl` headers have no `.spv`, by design.** They are
  `#include`d via `-I` and appear only in `DEPENDS`. A probe that reports
  `spv MISSING` for `common_base.glsl` has found the correct build, not a
  broken one.
- **Derive the `.spv` name as `$(basename $f).spv`, not `${f%.*}.spv`.** The
  staged names (`splat.vert`, `common_base.glsl`) otherwise lose their suffix
  and every file reads as missing. This is what CMake's
  `get_filename_component(NAME)` produces.

And the standing rule from the quirk that started this: a shader edit does not
change `build/voxelforge`. **Never render or measure inside another session's
announced shader window** — you snapshot a half-applied tree and get
plausible, wrong pixels. Confirm `.spv` identity, not binary health.

## Measured (2026-10-08): the decomposition, and what it does *not* prove

Provenance recorded **before** the first render: `build/voxelforge`
md5 `07306f1c16870fc6a2b1d33433d48a53`; `surfelize.cpp`
`c964aea73d990b7d9be7e3ef08e4b5d8`, `common_base.glsl`
`2c44b42cb40f7b94cc72a3e8ff192c69`, `common_splat.glsl`
`82ff8975926646a72f1c05efb78394e4`, plus all 18 `build/shaders/*.spv` md5s.
Three arms, identical `--cam 1.0,2.0,1.5,5.3,1.0,11.3` at 1280×720, each with its
own `VF_OVERLAY_PATH`. The log pattern is `[%H:%M:%S.%e]`
(`src/core/log.hpp:9`), so the boundaries are already timestamped — **no rebuild
is needed to time this.**

| component (ms) | A: no override | B: night −30 | C: day 34 |
|---|---:|---:|---:|
| `voxel_field` EDT | 3080 | 3064 | 3307 |
| read records | 750 | 751 | 755 |
| SVO synthesize | 3853 | 3883 | 4089 |
| **sun-independent subtotal** | **7683** | **7697** | **8151** |
| surfelize enum+surface | 260 | 255 | 251 |
| surfelize shade+bucket | 2935 | 2893 | 2879 |
| **surfelize subtotal** | **3195** | **3148** | **3131** |
| surfelize → splat upload | 655 | 857 | 673 |
| **sun-dependent subtotal** | **3850** | **4005** | **3804** |

Sun-independent work is **~2× the sun-dependent work**. So a
`rebuildSurfels`-direct path should turn a ~11.5–12 s reload into ~3.8–4.0 s.

### Three things this table does not prove

1. **It is not the mid-run stall.** `VF_TEST_SUN_PHASE` and `VF_TEST_SUN_TIME`
   both fire inside `App::run` *before* `initWindow`/`initVulkan`
   (`run.cpp:120-131`), and there is no mid-run sun hook in the 20 `VF_TEST_*`
   keys. The "sun changes on an already-loaded world" latency is **unmeasured**.
   What transfers is the component costs — a reload invokes exactly these
   functions. The 3× figure above is **arithmetic, not an observation**.
2. **Total wall time is not comparable across arms.** Arm B was the first run
   and paid a cold cache in the atlas phase (9.79 s vs 0.39 s warm). Only the
   component rows are consistent; never quote seconds-per-run from this set.
3. **A post-shot anomaly is unexplained.** Arms B and C perform a **second**
   full `VoxelField` build (~3.1–3.2 s) after the shot is written; arm A does
   not. This is *consistent with* `setSunPhase`'s `requestWorldReload`
   deferring and landing as its own reload rather than coalescing with the
   initial load — but there is no log line at `requestReload`/`consumeRebuild`,
   and no overlay lines appear in any arm, so the mechanism is **not
   established**.

### The finding that should size the shadow-map work

**`shade+bucket` is 2879–2935 ms in all three arms.** At −30° the
`dot(n, sunDir) > 0.02` backface gate skips `shadowMarch` for the large
majority of up-facing surfels, and the cost did not move — within 2% of the
full-march day arm.

That bounds `shadowMarch`'s share of the 2.9 s as **small**: if the march were
a large fraction, removing it for most surfels would have shown up as a drop.
So the bake's ~2.9 s is writing and bucketing 4.19M surfels, not shadowing.

Two consequences, both about sizing rather than about correctness:

- A sun-space shadow map does **not** buy CPU bake time. Its case is
  interactivity and quality, not the bake.
- A `rebuildSurfels`-direct path still pays ~2.9 s, so **that is the floor for
  an interactive sun slider** unless the surfel write/bucket cost comes down
  too. Decide this before sizing the effort.

### The measurement seam (written, not yet verified)

There was no way to time the mid-run stall headlessly, and that gap was itself
the finding — it is what a day/night slider needs for a test.
`VF_TEST_SUN_PHASE_DEFERRED="day"|"night"` now exists in
`runPreInputTestHooks` (`src/app/frame/run_hooks.cpp`), so it runs *before*
`pollWorldAndTextures` and the reload is consumed the same frame. It gates on
the first frame that is both past the load (`m_frameIdx >= 20`, the threshold
`VF_GUI_TEST` already relies on for a reload trigger) **and** `m_layers.loaded()`,
with a one-shot latch so a late load cannot make it silently never fire. It calls
the same `setSunPhase` as the preset buttons and the `P` hotkey.

Read the latency as the delta from the `VF_TEST_SUN_PHASE_DEFERRED … at frame N`
line to the **following** `splat backend:` line — both already carry
`[%H:%M:%S.%e]`, so no extra instrumentation is needed.

**Status: MEASURED.** First trigger→completion delta taken 2026-10-08 with
`VF_TEST_SUN_PHASE_DEFERRED=night --smoke 400`: **7.922 s**, summing exactly
across its five segments.

| segment | s | class |
|---|---:|---|
| kickoff (poll → reload request) | 0.826 | neutral |
| `VoxelField` EDT | 3.171 | **sun-independent** |
| SVO synth + read | 0.944 | **sun-independent** |
| surfelize re-bake | 2.334 | sun-dependent |
| surfel upload | 0.647 | sun-dependent |

**Sun-independent 4.12 s (52%) / sun-dependent 2.98 s (38%).**

### What this retires, and what it corrects

**The "second `VoxelField` build" was never an anomaly.** It is finding 1
landing in-window: the sun change re-runs the field EDT, and 3.171 s of the
7.922 s — 40% — is work with no sun in it. An earlier note here described that
extra build as unexplained; it was this, made invisible by short runs exiting
before the second rebuild finished.

**The `3×` prediction for `rebuildSurfels`-direct was too high. It is 2.1×.**
Deleting the sun-independent work leaves 2.98 s + 0.83 s kickoff ≈ **3.8 s**, so
7.92 → ~3.8 s. The earlier figure summed *startup* component costs, and startup
pays first-touch costs a warm reload does not (here SVO + read is 0.94 s against
4.57 s cold). The direction held; the multiplier did not.

### CORRECTION (2026-10-08): `shade+bucket` is ~2.9 s cold / ~2.1 s warm, AND ~0.8 s of it IS the sun shadow march

An earlier version of this section claimed the bake's sun-independence was
*measured* — that `shade+bucket` was flat across sun states (2935 / 2893 /
2879 ms) and therefore `shadowMarch` was a small share. **That reading was
confounded and is retracted.** The three arms were three *separate processes*,
and the night arm was the **first run of its session** — cold. A cold first run
inflates `shade+bucket` by roughly the same magnitude as the night saving, the
two cancelled, and the cancellation was read as "no effect". The cold-cache
confound was noticed and recorded at the time, and then not carried into the
interpretation of the number it was corrupting.

Two independent measurements put the real sun-state effect at **~0.8 s**:

| source | day | night | delta |
|---|---:|---:|---:|
| warm-controlled arms (perf session, same tree) | 3418 / 3503 / 3445 | 2619 | ~830 ms |
| one process, `VF_TEST_SUN_PHASE_DEFERRED` re-bake | 2933 (first bake) | 2078 (deferred) | ~855 ms |

Different processes, different methods, same magnitude. At −30° the
`dot(n, sunDir) > 0.02` backface gate skips `shadowMarch` for the large majority
of up-facing surfels, and that is worth ~0.8 s.

**What survives from the old text:** the 2933 → 2078 drop *within one process*
was still partly a warm-up artefact (first bake versus second bake), so it is not
a clean sun measurement either — it just happened to land near the right answer
for the wrong reason. And the practical sizing point holds in the direction it
pointed: an interactive day/night slider is the **warm** case, so the re-bake
cost to beat is ~2.1 s, of which ~0.8 s is shadowing.

**Retracted outright:** "shadowMarch is small, so shadow cost is not your
lever". It is not a small term.

Note the shape of the mistake for whoever reads this later: the claim was not
made up, it had a table of real numbers behind it, and it was still wrong —
because the numbers came from arms that were not comparable, and comparability
was the thing that was never checked.

So the shape stands:

- **shadow map ON** — a sun change is a GPU depth-map re-render, with **no**
  surfel rebuild at all.
- **shadow map OFF** — today's path, with `rebuildSurfels`-direct taking it from
  7.92 s to ~3.8 s, leaving ~2.1 s (warm) as the floor.

### Reproducing the measurement

```
VF_TEST_SUN_PHASE_DEFERRED=night \
VF_OVERLAY_PATH=/tmp/deferred.vxw \
./build/voxelforge --smoke 400
```

Read the delta from the `VF_TEST_SUN_PHASE_DEFERRED … at frame N` line to the
**following** `splat backend:` line.

- **Do not set `VF_TEST_SUN_PHASE` at the same time** — it fires before
  `initWindow` and the delta would be against an already-shifted sun.
- **Set `VF_OVERLAY_PATH`** — a headless run without it still *writes* the
  overlay ([[concepts/overlay-silent-write-trap]]).
- If the trigger appears but the following `splat backend:` does not, the run
  outran the reload — raise N, do not conclude the gate misfired.

The gate was a magic `m_frameIdx >= 20` inherited from `VF_GUI_TEST` and is now
"the first frame after the first loaded frame" (arm on the first
`m_layers.loaded()` frame, fire on the next), which works at any run length.

Cross-links: [[concepts/sun-direction-pipeline]] (the shading lane and the one
elevation→direction conversion this must not fork),
[[concepts/load-time-field-build]] (the 13 s this avoids),
[[concepts/detail-pipeline]] (the `U` toggle's identical re-bake obligation),
[[concepts/live-edit-surfel-parity]] (why a re-bake must reuse the bake's
normal rule verbatim — the overlay caveat above is the same class of problem),
[[concepts/baked-sun-shadow-contract]] (the CPU bake rule itself — the
`shadowMarch` two-phase march, the `dot(n, sunDir) > 0.02` backface skip, the
pass-3 penumbra/AO neighbour averaging and child inheritance; this page is only
the cost side, and any shadow-map replacement has to reproduce *that* rule),
[[concepts/measurement-discipline]] (why the table is empty rather than filled
with a plausible estimate).