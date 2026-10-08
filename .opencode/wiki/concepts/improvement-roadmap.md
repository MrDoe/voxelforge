---
title: Rendering engine improvement roadmap
tags: [roadmap, performance, quality, verification]
sourceRefs: [AGENTS.md, docs/rendering.md, .opencode/wiki/concepts/load-time-field-build.md, .opencode/wiki/concepts/splat-base-seal.md, .opencode/wiki/concepts/dynamic-sun-shadows.md, .opencode/wiki/concepts/night-gate-thresholds.md, .opencode/wiki/concepts/interactive-ui-coverage-gap.md, .opencode/wiki/concepts/demo-capture.md]
lastReviewed: 2026-10-08
---

# Rendering engine improvement roadmap

Prioritized list distilled 2026-10-08 from the wiki + `docs/rendering.md`.
Each item cites its measuring page so a future session can pick up the
instrument, not just the slogan.

## P1 — Finish the tile splat path (`VF_TILE=1`)

`docs/rendering.md`: the compute-only tile pipeline (bin/scan/base/render,
`splat_tile_*.comp`) renders the full scene; water is bit-exact vs the
forward path; opaque is close in isolation but the full composite still
shows small per-component fp deltas through the blend chain. AGENTS.md
quotes **143 ms vs 57 ms** at 720p hero (bin/fill 65 ms incl. contended
atomics + two projection passes; render 78 ms from full-tile per-pixel
loops) — **doc-sourced, not re-measured**; a provenance-tracked
re-measure with `VF_TRACE` is pending (Vega). The re-measure's protocol:
forward and tile arms on the **same binary and same build, back to
back**, with that pair's own render-to-render noise floor reported
before any forward-vs-tile delta — otherwise the delta is
indistinguishable from noise (the failure mode dismantled elsewhere
today). Forward remains the default.

Canonical measured status now lives at [[concepts/tile-splat-parity]];
the summary below is kept in sync.

**MEASURED 2026-10-08 (Vega, same binary, back to back, provenance
pre-recorded):** the "small fp deltas" wording in AGENTS.md is **stale**.
Same-binary controls: forward-vs-forward mean\|d\| 0.0467/255 (3.93% px
differ), tile-vs-tile 0.0547/255. Forward-vs-tile: **mean\|d\|
5.5624/255, 64.31% of px differ, 47.64% >2/255, max 153, ≈119× the
pair's noise floor** (R 6.21 / G 5.86 / B 4.62). Localisation: sky at
noise floor (0.136/255), non-sky 10.13/255, uniform top-to-bottom
(0.92 / 10.91 / 10.90), signed luma −1.0/255, tile darker on only 44.2%
— i.e. **"no net bias, high per-pixel variance, everywhere on geometry,
sky clean"**, the signature of a *different set of contributing disks*,
not different arithmetic. The earlier seal-equality (`fragDepthQ ==
depth`) explanation was **refuted** by this localisation. Next untested
candidate: base-seal threshold parity (`uSplat3.x` from
`VF_SPLAT_SEAL_ALPHA`, default 0 = seal everything) — if the tile path
receives a different default, coverage changes across all geometry with
no brightness bias, matching the measurement. Also open: `dups 0` at
80×45 tiles for the near band. Timings (143/57/65/78) remain
**unre-measured**: `VF_TRACE` emits nothing in a `--shot` run (3
frames), so they must come from `--smoke N` or the interactive HUD.
Once parity is root-caused and fixed, the gate is the pinned 0.18/255
same-binary noise floor from `[[concepts/splat-edge-fade-measurement]]`,
never bit-exactness; the honest value proposition is determinism, not
speed.

## P1 — Load-time EDT batching (VoxelField::build, ~13 s of ~17.6 s)

`[[concepts/load-time-field-build]]`: 72k object components, 533k cells,
but **361M padded bbox cells** (~680× inflation) because every component
pays its own `2*kPad`-expanded bbox EDT. Code state (verified 2026-10-08 in
`src/voxel/voxel_field.cpp`): per-component outputs are already
content-hash cached (`m_prevHashes`/`m_prevOuts`, order-independent
`compHash`) and the Dijkstra transform already runs parallel over
components, so small edits only re-pay for changed neighbourhoods. What is
**not** done: spatially merging small nearby components into one EDT group.
The dominant cost is thousands of 1-cell grass/pebble parts each paying a
`(2kPad)^3`-ish grid all to itself. Batching must preserve (a) the 680×
inflation bound per merged group (the 32M-cell safety valve), (b) per-comp
output isolation in `CompOut` (consumers key on component), and (c) the
content-hash cache key (merged group = hash of member comp hashes). Do not
shrink `kPad` — it carries the stored SDF air band. Work sequencing
(2026-10-08): George declined the prototype until his shadow-map
feature lands (red texture gate + never-run night gate + approved
shadow-map work queue); offer reactivates on his ping. Vega's queue is
the irradiance volume.

## P1 — Surfelize bake is 4.3 s and shade+bucket dominates (measured)

Victor's corrected numbers (2026-10-08): `surfelize` bake
**3418 / 3503 / 3445 ms** warm-day across three runs, **2619 ms** at
night; the 4278 ms figure was a **cold run**. Of the warm cost,
shade+bucket ≈ 4006 ms / 4.19M surfels on the cold-profile run —
instrument to re-baseline before quoting it as current.
`--smoke 60` averages **5.52 ms/frame** at 640×360; `test-surfel`
4/4 green. The HEAD-control comparison is **invalid on a dirty tree**,
so no isolated win is claimed for the landed frame-CPU env-cache +
bake AO-hash. Open lever: the surfel stream is rebuilt on every world
reload (`rebuildSurfels`), so the bake's shade+bucket pass is the next
candidate for chunking/parallelism the same way the field build was
cached. Load-profile breakdown (EDT vs surfelize vs upload) still
pending — this page takes it when it lands.

## P2 — Splat silhouette holes: coverage count in the stencil

`[[concepts/splat-base-seal]]`: the edge window buys nothing alone
(1.00×); the seal decline is the only thing that dissolves a silhouette
(1.28×) and the only thing that costs interior fidelity (+5.6/255). The
planned fix is a coverage count in the already-allocated, still-unused
D24_S8 stencil so a translucent rim can distinguish "covered by base
fragments" from "sky bleed".

## P2 — Dynamic sun / night correctness

- `[[concepts/dynamic-sun-shadows]]`: sun change stalls ~13 s on a field
  EDT that has no sun in it. Re-bake shortcut exists; GPU march measured as
  slower **and** less accurate than the 10 cm bake. The scratch-clone A/B
  harness is built; the numbers are **still unrecorded** — record them.
- `[[concepts/night-gate-thresholds]]`: 14 emissive-derived lights are not
  day/night gated, so both endpoints of the 0.10–0.27 ratio band were
  measured against a tree that no longer exists. Re-derive the band.
- `[[concepts/sunset-dimming]]`: the sky does not dim through golden hour
  (luma 164 at noon → 172 at elev 4); three causes in `skyColor`, and no
  reference shot frames a sunset.

## P3 — Verification gaps

- **Wiki lint: presence is not uniqueness** (Vega, 2026-10-08): the
  standard whole-tree wiki check verifies every page is *linked*, not
  that it is linked *exactly once*. It reported "total dangling: 0" on a
  tree with a duplicate index entry — a false PASS, the hard-to-spot
  failure mode (a false alarm gets investigated; a green result gets
  trusted). Rule to add: occurrence count exactly 1 per page, scoped
  **within the Pages section** — the "How to navigate" section
  deliberately re-cites Pages entries, so a whole-file count reports
  ~35 false duplicates.
- **A third category: presence vs settledness** (Wendy, 2026-10-08): a
  claim written before the thing it describes is settled gets filed as a
  claim, not as a hedge — and a hedge would not have saved it either,
  because the wrongness was in the mechanism, not the confidence. Her
  bridge line to the landed `resampleRecords` was approved, then
  retracted on reread: it assumed the normalizing load *preserves* fine
  geometry, but it resamples *into* the world lattice, so it sidesteps
  the coverage problem by giving up the fine geometry. Lint rule: a
  cross-reference to another session's in-flight build must say
  "proposed/pending", not describe it as done.
- `[[concepts/interactive-ui-coverage-gap]]`: `drawHud()` runs only in the
  interactive path; `--shot` renders omit the sidebar entirely. Fix sketched:
  `VF_TEST_PANEL=edit` calling `drawHud()` once from a headless run.
- `[[concepts/demo-capture]]`: `--mode dual` is verified for a complete
  single-backend pass; the two-backend concatenation is not produced end to
  end.

## P3 — Photorealism chain polish

Fog (J) / motion blur (K) / DoF (L) stay opt-in; SSR+SSAO+detail-normals
cost is measured (+0.65 ms hero 720p, +1.34 ms 1080p). No bloom/GI lane.
Fog density is a simple water-table falloff with fbm patchiness.

Cross-links: [[concepts/load-time-field-build]], [[concepts/splat-base-seal]],
[[concepts/dynamic-sun-shadows]], [[concepts/night-gate-thresholds]],
[[concepts/interactive-ui-coverage-gap]], [[concepts/demo-capture]],
[[concepts/measurement-discipline]], [[concepts/measurement-provenance]].
