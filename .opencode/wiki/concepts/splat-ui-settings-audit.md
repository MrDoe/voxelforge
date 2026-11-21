---
title: Splat UI settings audit (what actually changes the picture)
tags: [splat, ui, rendering, measurement, A/B, surfels, no-op]
sourceRefs: [src/app/ui/panel_render.cpp, src/render/splat_pass.hpp, src/render/splat_pass.cpp, shaders/splat.vert, shaders/splat.frag, shaders/splat_cull.comp, shaders/splat_tile_bin.comp, shaders/splat_tile_render.comp, src/voxel/surfelize.cpp, src/app/app.hpp, tests/screenshots/splat_ui_audit_core.png, tests/screenshots/splat_ui_audit_edges.png]
lastReviewed: 2026-11-20
---

# Splat UI settings audit (what actually changes the picture)

Measured 2026-11-20 against `build/voxelforge`, scenery `LakesideHamlet`, the
`visual_check` hero camera (`-16 6.5 -14 → 6.5 0.8 11`), 480×270, `--animtime 0`,
`VF_NO_OVERLAY=1`, a scratch `VF_OVERLAY_PATH`, and a frozen alpaca pose. Every
case is one `--shot` with one env knob changed from the same binary.

**Noise floor is 0 px, not ~1.25 %:** two identical `--shot` renders are
byte-identical (headless TAA is off and the clock does not advance). The 1.25 %
floor applies to live/HUD frames, not to this harness. That makes even a 0.4 %
pixel delta measurable.

## The SURFELS group, mapped to what it drives

| UI control | Code / uniform | Applied | Measured effect (hero, vs default) |
|---|---|---|---|
| **Splat radius** | `m_radiusScale` → `uSplat2.x` (`setRadiusScale`) | per frame, no rebuild | 0.5: **43.7 %** px >2, 17.0 % >12; 2.0: **49.6 %** px >2, 21.4 % >12 |
| **Edge blend** | `setEdgeBlend` sets **both** `uSplat2.w` (window) and `uSplat3.x` (seal decline) | per frame, no rebuild | 0.5: **35.9 %** px >2; real silhouette change, but 45 % of changed pixels are blue-dominant (sky) — see [[concepts/splat-base-seal]] |
| **Sharp-edge fit** | `m_edgeShrink`, baked radius | world/surfel rebuild | 0.25 → 0.35: **0.97 %** px >2 (0.27 % >12), confined to object regions |
| **Interpolate crease splats** | `m_edgeFill`, bridge emission | rebuild | bridges off vs on at shrink 0.35: **0.59 %** px >2 here (much larger at a cabin corner — see [[concepts/edge-aware-surfel-radius]]) |
| **Crease splat size** | `m_edgeBridgeSize`, bridge/cap radii | rebuild | **0 px with fill off**; with fill on, 1.0→0.5: 0.36 %, 1.0→2.0: **1.54 %** px >2 |
| **Fill corner splats** | `m_cornerFill`, cap emission | rebuild | **0 px** with fill off *and* with fill on at this view: the coverage guard leaves ~557 caps among ~3.4 M surfels |
| **Exposure** | `m_exposure` → post tone map | per frame | real, but not a splat setting at all |

The code default is `m_edgeShrink = 0.25` (`src/app/app.hpp`); `AGENTS.md` and
the older wiki page quote 0.35. There is no `ui` block in the shipped
`LakesideHamlet/world.json`, so 0.25 is what a default launch actually bakes.

Env-only splat knobs (not in the panel) are also live: `VF_SPLAT_SIGMA=0.3`
moved **9.6 %** of pixels, `VF_SPLAT_OPACITY=0.5` **14.5 %**,
`VF_SPLAT_EXTENT=1.5` **34.7 %**. They change the picture but are invisible to
a UI-only user.

## Why "not all splats resize" when the size slider moves

1. **Every real surfel does scale.** `splat.vert` multiplies `rU/rV` by
   `rScale` and passes the scaled radii in `vRadii`; `splat_cull.comp` and the
   tile bin/render shaders apply the same clamp. Base parents, LOD rings,
   water-grid surfels, edge bridges and micro children all go through it. The
   A/B confirms it: ~44–50 % of hero pixels and 51–63 % of an alpaca close-up
   change between 0.5 and 2.0.
2. **The sky is not a splat.** It is a fullscreen triangle (`SKY_MODE=1`).
   Classifying the hero baseline: only **3.2 %** of sky-class pixels changed
   under radius 2.0, against **82.2 %** of green-foliage pixels and **65.3 %**
   of terrain/object pixels. The unchanged upper band in a diff is the sky,
   not a broken slider.
3. **Several visible layers are not splats at all.** Micro objects are
   sphere-traced/mesh actors with their own pass; fur is hair cards; photo
   foliage cards and grass are raster cards. None read `uSplat2.x`. Isolating
   the alpaca with the `VF_FUR=0` escape hatch (5,757 coat pixels) only
   **12.3 %** of those pixels changed under radius 2.0, versus 62.8 % of the
   full close-up frame — the animal does not grow or shrink with the slider.
4. **Dense interiors are already saturated.** The base pass seals the nearest
   fragment and the depth band rejects everything behind `nearest + tol`, so
   enlarging overlapping 1.4-cell disks mostly extends silhouettes; the
   interior can look unchanged even though every disk grew. This is a
   compositing effect, not a per-splat exemption.
5. **Backend scope:** `VF_SPLAT_RADIUS=2.0` under `--mode svo` is
   **byte-identical** (0 px) to baseline. The control belongs to the Gaussian
   backend only, exactly as the sidebar's SVO hint says.

## Which controls buy no visual advantage

- **Fill corner splats** — no measurable change on the shipped hamlet, with
  crease fill off *or* on. The mechanism is live (a thin rod emits caps, pinned
  by `tests/test_micro.cpp`-style fixtures in `test_surfelize`), so this is a
  property of the content, not dead code. It is not worth exposing as a
  general-purpose "look" control.
- **Crease splat size** while *Interpolate crease splats* is off — a strict
  no-op: bridges/caps are emitted inside `if (edgeFill && edgeShrink > 0)` in
  `surfelize.cpp`. The slider should be disabled or annotated in that state.
- **Edge blend's window half by itself** — the measured silhouette bleed gain
  is 1.00× because the base seal covers the faded rim; only the seal-decline
  half changes silhouettes, at an interior-fidelity cost. See
  [[concepts/splat-base-seal]] and [[concepts/splat-edge-fade-measurement]].
- **Sharp-edge fit / crease interpolation** are worth keeping: small at the
  hero view, but load-bearing at cabin corner posts (2–3 cells wide), where
  the shrink opens a real hole the bridge seals.

## Reproducing

```sh
VF_SCENERY=LakesideHamlet VF_NO_OVERLAY=1 \
VF_OVERLAY_PATH=/tmp/opencode/splat_audit_overlay.vxw \
VF_TEST_BEHAVIOUR_FREEZE='alpaca:0,0,0:0:0:0' \
  ./build/voxelforge --shot /tmp/out.ppm \
  --cam -16 6.5 -14 6.5 0.8 11 --width 480 --height 270 --animtime 0
```

Change exactly one env knob per run; compare with numpy on the PPM bytes
(`VF_SPLAT_RADIUS`, `VF_SPLAT_EDGE` + `VF_SPLAT_SEAL_ALPHA`, `VF_EDGE_SHRINK`,
`VF_EDGE_FILL`, `VF_EDGE_SIZE`, `VF_CORNER_FILL`, `VF_SPLAT_SIGMA`,
`VF_SPLAT_OPACITY`, `VF_SPLAT_EXTENT`). Always render the untouched baseline
twice first: a non-zero "noise floor" here means the harness is wrong, not the
setting. Example contact sheets: `tests/screenshots/splat_ui_audit_core.png`,
`tests/screenshots/splat_ui_audit_edges.png`, the radius-change mask
`tests/screenshots/splat_radius_change_mask.png`, and the captured panel
`tests/screenshots/splat_ui_render_panel.png`.

## Fixes attempted (2026-11-20)

**Landed — corner caps decoupled from crease fill.** `buildSurfels` and
`appendCandidateEdgeBridges` had both caps and bridges inside
`if (edgeFill && edgeShrink > 0)`, so *Fill corner splats* was a strict no-op
whenever crease interpolation was off. A cap repairs a corner the parent disk
misses (including thin shells); a bridge repairs a crease on a tightened
parent. They are now independent gates (both still require `edgeShrink > 0`),
with bake/live-store parity. `tests/test_surfelize.cpp` gains a case that fires
caps with `edgeFill=false` and verifies zero caps when `cornerFill=false`;
`unit_surfel_tests` passes. The shipped hamlet still shows no visible cap
change — the coverage guard leaves ~557 caps in 3.4 M surfels — so the win is
correctness and a control that is no longer wired to nothing, not a new look.

**Landed — UI honesty.** *Crease splat size* is now disabled when neither fill
mechanism is active (previously it sat there moving a number while emitting
nothing); *Fill corner splats* is disabled only when `edgeShrink == 0` and its
tooltip now states the real dependency.

**Refuted again — stencil coverage count.** The dormant
`VF_SPLAT_COVERAGE` path hardcodes `stencil reference = 8` on a count of every
disk covering the pixel. A tunable reference (`VF_SPLAT_COVERAGE_REF`) was
added and swept 0/1/2/4/8 against a byte-identical baseline: **all six renders
were 0 px**. Even reference 0 (seal wherever any disk covers) changes nothing,
so every covered pixel — silhouette included — has a count above every
threshold. The experiment was reverted; the path stays opt-in and inert,
consistent with [[concepts/splat-base-seal]]. A count of *all* overlapping
disks cannot distinguish a lone silhouette disk from an interior surface.

**Verification.** `unit_surfel_tests` passes with the new case. The main-tree
binary was temporarily unusable for rendered A/B: an unrelated in-flight
session made every headless `--shot` segfault in `vf::ai::parseUrl` ←
`OpenCodeClient::connect` ← `ChatUi::init` before any rendering (`VF_LLM_URL`
did not bypass it). I rebuilt the edited surfelizer in an isolated worktree at
`HEAD` and measured there: default vs `VF_CORNER_FILL=1` is still **0 px** on
the hamlet — the coverage guard suppresses caps on covered convex corners, so
the fix restores control semantics without changing the shipped look. The
default path remains byte-identical to baseline, i.e. no regression. The two
`test-surfel` visual sub-gates (`fast_visual_check` ownership-blue,
`fast_live_edit_check` edit-too-small) failed for reasons outside these
changes — both paths are gated off by default in this edit.

See also [[concepts/edge-aware-surfel-radius]], [[concepts/render-defaults-off]],
[[concepts/splat-base-seal]], [[entities/hud-sidebar]].
