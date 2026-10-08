---
title: Tile splat path vs forward — measured parity status (2026-10-08)
tags: [splat, tile, parity, measurement, deterministic]
sourceRefs: [AGENTS.md, docs/rendering.md, .opencode/wiki/concepts/splat-edge-fade-measurement.md, .opencode/wiki/concepts/improvement-roadmap.md, tools/silhouette_check.py]
lastReviewed: 2026-10-08
---

# Tile splat path vs forward — measured parity status

`VF_TILE=1` runs the compute-only tile pipeline (`splat_tile_{bin,scan,base,render}.comp`)
instead of the forward rasterisation passes. AGENTS.md documents the
parity status as "water bit-exact, opaque close in isolation, full
composite shows small per-component fp deltas" — **that wording is
stale**. The measured status (Vega, 2026-10-08):

## Provenance

- Forward vs `VF_TILE=1`, hero camera (−16 6.5 −14 6.5 0.8 11), **1280×720**
  (not the 720p "hero" resolution assumed elsewhere).
- Same binary, arms back to back, provenance recorded **before** the first
  render (binary md5, all 18 `.spv` md5s, exact command, metric definition).
- Metric: mean |d| per pixel, fraction of pixels differing, fraction over
  thresholds, max |d|, per channel.

## Numbers

| comparison | mean\|d\| /255 | px differ | >2/255 |
|---|---|---|---|
| control: forward vs forward | 0.0467 | 3.93 % | (0.67 %) |
| control: tile vs tile | 0.0547 | 4.21 % | (0.81 %) |
| **forward vs tile** | **5.5624** | **64.31 %** | **47.64 %** |

- max |d| 153; forward-vs-tile mean ≈ **119×** the same-binary control floor.
- Per channel: R 6.21 / G 5.86 / B 4.62.
- Localisation: sky (25.6 % of frame) at 0.136/255 — essentially at the
  control floor; non-sky (74.4 %) at 10.134/255. Uniform top-to-bottom:
  top 0.92 / middle 10.91 / bottom 10.90. Signed luma −1.0/255, tile
  darker on only 44.2 % — no exposure bias.

## Interpretation

"No net bias, high per-pixel variance, everywhere on geometry, sky clean"
is the signature of a **different set of contributing disks** reaching each
pixel — not different arithmetic and not a quantisation edge. Every
geometry pixel gets a different coverage set; the sky, which has no disks,
is unaffected.

**REFUTED:** the seal-equality mechanism (`fragDepthQ == depth` on the
1e-5-quantised depth) first proposed as the dominant cause. The
localisation numbers contradict it — if a fragment-within-one-quantum of
the seal band were the cause, the delta would hug depth discontinuities
(foreground silhouettes), not spread uniformly. Do not carry that
explanation forward.

Current untested candidate: base-seal threshold parity between paths —
`uSplat3.x` comes from `VF_SPLAT_SEAL_ALPHA` (default 0 = seal
everything); if the tile path receives a different default, coverage
changes across all geometry with no brightness bias, which fits the
measurement. Also open: `dups 0` at 80×45 tiles, which may be legitimate
for far geometry but should be confirmed for the near band.

## What is known about water

Water remains **bit-exact** between the paths: it tests
`fragDepthQ < depth` (strict, never equality) and every surviving water
fragment contributes identical colour and alpha, so its accumulation is
order-independent. Opaque differs because per-surfel colour/alpha vary and
the seal/coverage set differs.

## Timings — unverified

AGENTS.md's **143 ms tile vs 57 ms forward** at 720p hero (bin/fill 65 ms,
render 78 ms) is **doc-sourced only**: `VF_TRACE` emits nothing in a
`--shot` run (3 frames, no profiler output), so that split must be
re-measured with `--smoke N` or the interactive HUD before it is quoted.

## Gate discipline

- The tile path's internals are deterministic (fixed submission order,
  entry-private cursors) and arguably *more* reproducible than the forward
  path, whose fragment accumulation order is hardware/bin-dependent. Its
  honest value proposition is computing-side **determinism**, not speed.
- The parity gate for this path must be the pinned same-binary noise floor
  (see `[[concepts/splat-edge-fade-measurement]]`: establish the
  forward-vs-forward floor first, never quote a raw backend delta as
  evidence). Bit-exactness with the forward path is **not** an achievable
  gate — the forward path is not bit-reproducible against itself.

## Forward path remains the renderer

Until per-scene parity (hero/corner/overview/house/water) holds under the
noise-floor gate, `--mode splat` stays the default and `VF_TILE=1` is a
diagnostic/reproducibility lane. Next measurement owner: Vega.

Cross-links: [[concepts/improvement-roadmap]],
[[concepts/splat-edge-fade-measurement]],
[[concepts/splat-base-seal]], [[concepts/measurement-provenance]].
