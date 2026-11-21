---
title: Framerate triage — startup, GPU stages, and GPU contention
tags: [performance, framerate, profiling, gpu, contention, triage]
sourceRefs: [src/app/frame/profiler.cpp, src/app/frame/record_headless.cpp, docs/rendering.md, AGENTS.md, .opencode/wiki/concepts/load-time-field-build.md, .opencode/wiki/concepts/fur-raster-parity.md, .opencode/wiki/concepts/tile-splat-parity.md, .opencode/wiki/concepts/improvement-roadmap.md]
lastReviewed: 2026-11-18
---

# Framerate triage

"Why is the framerate so low?" has three different answers depending on *which clock* is slow. Measure first, then pick the row — the history of this project is full of confident diagnoses that a timestamp query or a `pgrep` refuted.

## The measured answer that triggered this page (2026-11-18)

Peer measurement (Vega), 960×540, `--smoke 360`, default camera, same binary:

| mode | `geo` (frames 120/240) | `post` | `fx` | `taa`/`tail` | wall avg |
|---|---|---|---|---|---|
| `--mode splat` (default) | 9.23 / 9.27 ms | 0.06 | 0.32 | 0.00 | **9.55 ms** (~100 fps) |
| `--mode 3dgs` (OIT), before | 28.72 / 29.36 ms | 0.06 | 0.33 | 0.00 | **29.93 ms** (~33 fps) |
| `--mode 3dgs` (OIT), after cheap OIT fragment colour (`d36949e`) | 11.09 / 10.86 ms | 0.06 | 0.33 | 0.00 | **11.39 ms** (~88 fps) |

Provenance: `/tmp/opencode/fps-default.log`, `/tmp/opencode/fps-3dgs.log` (before) and `/tmp/opencode/fps-3dgs-real.log` (after), each ending `smoke done: 360 frames` (avg/min/max), with `VF_TRACE` lines at frames 120/240. The default mode is **healthy** — ~9.5 ms at 960×540 is sensible against the 5.52 ms/640×360 baseline. The whole 3× gap was in `geo`; every other stage is negligible.

The follow-up refinement to the cause below: the second geometry pass was still needed, but the expensive part was its **fragment shader**, not the extra draw. `PASS_MODE 5` ran full `shadeSurfel()` on every surfel while the composite exposes that colour at only ~0.3 % sparse-coverage silhouette pixels. Cheapening that one branch recovered almost all of the gap (see SHIPPED section).

**Cause, verified in the code:** the `RenderMode::Dgs` branch in both `record_headless.cpp` and `record_interactive.cpp` shares the `Splats` block and calls `m_splatPass.record()` — the full depth prepass + opaque base seal + Gaussian band + water draw — then enters its own block and calls `m_splatPass.recordOit()` for a **second full pass over every surfel** into accum/reveal/backdrop (`PASS_MODE 5`, no depth rejection), followed by an `m_hdr → m_oitBase` copy and `m_oitPass.record()`. The first pass is not throwaway: it supplies the G-buffer and depth the FX chain/micro overlay read, and its depth-resolved `m_hdr` is copied to `m_oitBase` so solid interiors stay pixel-identical. The second geometry pass is the cost; the copy and the fullscreen composite are not. See [[concepts/3dgs-renderer]] for the mode's design and parity evidence.

A VRAM Tuner session was also reported active around this measurement — treat the absolute numbers as a single quiet-enough sample, not a noise-floor study (§1).

## SHIPPED (2026-11-18) — cheap OIT fragment colour

Commit **`d36949e`** (shader-only: `shaders/splat.frag`, +16/−3) cut `--mode 3dgs`
from `geo` 28.72/29.36 ms to **11.09/10.86 ms** at 960×540 — whole-frame avg
**29.93 → 11.39 ms**, now close to default splat's 9.55 ms.

Command and provenance (Vega):

```
VF_NO_OVERLAY=1 VF_OVERLAY_PATH=/tmp/opencode/fps-scratch.vxw VF_TRACE=1 \
  ./build/voxelforge --mode 3dgs --smoke 360 --width 960 --height 540
```

Before: `/tmp/opencode/fps-3dgs.log`; after: `/tmp/opencode/fps-3dgs-real.log`.

**Mechanism.** `PASS_MODE 5` (the OIT accumulation pass) used to call full
`shadeSurfel()` for every surfel — triplanar texture fetches, detail-normal
Sobel, `applyFlora`, 16-light shadow marches and the sky-visibility march —
even though `oit_composite.comp` exposes that colour only at the ~0.3 % of
pixels with sparse coverage. It now uses palette albedo × baked sun shadow/AO
for the OIT colour. The G-buffer, depth, base copy and composite are unchanged.

**Gate.** `dgs_check` PASS against a correct committed-HEAD baseline: default
splat 0.000–0.001, interiors ≤0.054/255, edges 0.02–0.29 %. The speed-up is
therefore a *shader-cost* result, not a quality trade measured only by eye.

## Pending (PROPOSED, NOT IMPLEMENTED as of 2026-11-18)

The owner of `--mode 3dgs` (Vega) had two structural options on the table.
The SHIPPED cheap-fragment fix above captured most of the measured win without
structural changes, so these are now lower priority; both touch files another
session (VRAM Tuner) has owned, so coordination precedes edits:

- **(a) skip the redundant part of `m_splatPass.record()` in the Dgs branch.**
  Constrained: `record()` supplies the G-buffer + depth the FX/micro chain
  reads, the `m_hdr` base copy that keeps interiors byte-identical, and the
  culled draw list. Only sub-passes whose output OIT does not consume could be
  removed; sky/backdrop overlap is the obvious small one.
- **(b) merge OIT accumulation into the band pass so surfels shade once.**
  Risk: the band pass tests `LESS` with a depth bias so only the front-surface
  band shades; OIT deliberately draws every overlapping disk with no depth
  test. Merging means giving up early-Z, which measured **55 → 5.9 ms**
  against full overdraw at a near-tree view — the reason for the two-pass
  design in the first place.

Not implementation facts: update this section only when a change lands and is
re-measured with the same 960×540 smoke + `VF_TRACE` protocol, and describe it
as shipped only then ([[concepts/measurement-provenance]]).

**Rejected angle — do not re-propose:** gating OIT fragment shading by a stencil
coverage count. `recordOit()` does shade every overlapping disk while the
composite discards interiors, which makes the idea look attractive, but the
mechanism was measured and **refuted** on 2026-10-21: the 1.4-cell disk radius
means a surface's own neighbours overlap everywhere (every terrain pixel is
multiply covered, so no count threshold separates interior from silhouette),
and Vulkan exposes no stencil read in the fragment shader. A custom
coverage-mask image would dodge the API objection but not the discrimination
one, and would add a whole extra geometry pass. See
[[concepts/splat-canopy-parity]] §"Why the coverage count in the stencil does
not work" and [[concepts/splat-base-seal]] §"The coverage count cannot be the
fix".

## 0. Get a number, and identify the clock

- **Interactive:** the HUD's `GPU ms` line, and `VF_TRACE=1`, which logs `gpu[N]: geo … post … fx … taa … tail … ms` every 120 frames (`App::accumulateProf`, `src/app/frame/profiler.cpp`; marks 0–5, harvested in `waitFrameSlot`). The split names the guilty pass; the total alone does not.
- **Headless:** `./build/voxelforge --smoke N --width 640 --height 360` prints avg/min/max ms at the end. A 3-frame `--shot` emits **no** `VF_TRACE` output, so any stage split quoted from one is doc-sourced, not measured (see [[concepts/tile-splat-parity]]).
- **Baseline to beat:** `--smoke 60` at 640×360 averaged **5.52 ms/frame** on the hamlet ([[concepts/improvement-roadmap]]). A much larger number on a comparable camera is a real regression; a small one is noise or contention.
- **"Every run takes ~18 s before the first frame" is not framerate.** That is the world load: ~17.6 s, of which 13.2 s is the per-component padded-bbox EDT in `VoxelField::build` ([[concepts/load-time-field-build]]). It is paid by every `--shot` and every test render; per-component caching means small edits do not re-pay it, but there is no cross-process cache. `ctest -j4` is the only wall-time lever — parallel renders do not help because the load already saturates the cores.

## 1. Check GPU contention before blaming the code

This is the highest-priority suspect on a shared machine, and a contended measurement is void **in either direction** (a contended PASS is not evidence either). Snapshot 2026-11-18 on this box: a `qwen3-vl-8b` llama-server held **7688 MiB**, an embeddings server 918 MiB, and VoiceStudio 238 MiB; no voxelforge process was running at that instant. That much VRAM held by a second client is the exact condition under which a windowed capture went dark while the offscreen `--shot` of the same camera stayed correct.

Checks, in order:

```
pgrep -af "build/[v]oxelforge"          # is a second renderer alive?
nvidia-smi --query-compute-apps=pid,used_memory --format=csv,noheader
```

Then **discard and re-run on a quiet GPU**; do not average a contended run into a conclusion. Never kill a process you did not start: a bare interactive `./build/voxelforge` is the user's session holding authored overlay state, and a peer's test driver owns its own GPU work. `crosstalk_peers`/`crosstalk_wait` are the coordination path.

## 2. Known cost centres, ranked by measured history

| suspect | when it bites | measured | lever |
|---|---|---|---|
| **`--mode 3dgs` (OIT)** | opt-in render mode | 960×540: pre-fix `geo` **28.7–29.4 ms**, whole-frame 29.93 vs splat 9.55; **fixed in `d36949e`** — cheap OIT fragment colour gives `geo` **11.09/10.86**, avg **11.39 ms** | shipped fix; `--mode splat` still marginally faster; [[concepts/3dgs-renderer]] |
| **Fur coat** | the alpaca is on screen | pre-fix `geo` **182.6 / 211.5 ms** — 98 % of the frame, 4.17 ms with `VF_FUR=0`; after the raster rewrite **~0.1 ms**, `geo` 5.5 ms ([[concepts/fur-raster-parity]]) | `VF_FUR=0` isolates it instantly |
| **Surfel overdraw** | close to trees/roofs | full per-fragment shadow/AO marches ~13 ms at 1080p (hence the CPU bake); near-tree 640×360 `geo` **55 → 5.9 ms** and 720p **113 → 10 ms** once no pass writes `gl_FragDepth` so early-Z bounds shading to the front band ([[concepts/splat-base-seal]], `docs/rendering.md`) | keep the depth prepass + EQUAL base + biased band intact; re-check any `gl_FragDepth` addition |
| **Tile path** | someone set `VF_TILE=1` | doc-sourced **143 ms tile vs 57 ms forward** at 720p hero; measured pixel parity only ~64 % of pixels ([[concepts/tile-splat-parity]]) | leave `VF_TILE` off — it is a determinism lane, not a speed win |
| **Optional FX / TAA** | SSR (5), SSAO (6), fog (J), motion blur (K), DoF (L) enabled | SSR+SSAO+detail-normals +0.65 ms hero 720p / +1.34 ms 1080p; TAA ships **off** ([[concepts/render-defaults-off]]) | `VF_RENDER_FLAGS=255` restores the pre-effects escape hatch |
| **Hi-Z occlusion / LOD** | always on by default | occlusion pyramid ~3 ms at 720p but saves fragments behind objects; the baked LOD rings are on unless disabled | `VF_NO_OCCL=1`, `VF_LOD=0` as A/B diagnostics |
| **Micro animation** | walking alpaca | the whole behaviour+clamp path **0.011 ms/frame** after the probe fixes — never the cost ([[concepts/micro-animation-cpu-cost]]) | none needed |

Other per-frame work that is real but smaller: the world-wide water-plane surfel grid, the derived emissive point lights (both backends march them), and the micro overlay + fur passes (both inert when no such layer is loaded).

## 3. Triage order

1. Read the `gpu[]` split. The largest stage names the next experiment.
2. `geo` dominant → A/B `VF_FUR=0`; then `VF_NO_GPU_CULL=1`, `VF_NO_OCCL=1`, `VF_LOD=0`; verify the depth prepass is running and no shader writes `gl_FragDepth`.
3. `post`/`fx` dominant → clear render flags, then re-enable effects one at a time. Note the photorealism chain runs in place after the tone map in the default order ([[concepts/photoreal-neural-postprocess]]).
4. Frame time fine but wall clock slow → it is the load (§0), not rendering.
5. Two runs of one camera disagree → contention or an unfrozen animation (§1); [[concepts/render-nondeterminism]] and [[concepts/micro-animation-cpu-cost]] cover the pinned-pose discipline. Do not tune against a noisy pair.

## Related

- [[concepts/vram-budget]] — renderer footprint measured ~0.89 GiB before the VRAM reclamation and ~0.55 GiB after; quantitative backing for the contention check in §1 (the foreign VLM worker holds ~7.7 GiB).
- [[concepts/3dgs-renderer]] — the opt-in OIT mode whose doubled geometry pass is the measured cause above.
- [[concepts/improvement-roadmap]] — the prioritized perf/quality queue and its instruments.
- [[concepts/fur-raster-parity]] — the clearest example of "profile, then fix" in this codebase.
- [[concepts/load-time-field-build]] — why scripts are slow even when frames are fast.
- [[concepts/measurement-discipline]] and [[concepts/measurement-provenance]] — what makes any of these numbers evidence.
