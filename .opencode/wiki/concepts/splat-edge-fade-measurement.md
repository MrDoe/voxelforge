---
title: Measuring a splat fragment change (noise floor first)
tags: [splat, measurement, methodology, visual-check, taa]
sourceRefs: [tests/visual_check.py, src/render/splat_pass.cpp]
lastReviewed: 2026-10-01
---

A splat fragment-stage change is easy to "measure" into a wrong conclusion.
Four traps, all hit on 2026-10-01 while fixing the silhouette edge.

**Establish the noise floor before comparing anything.** Two runs of the *same*
binary still differ: sub-pixel TAA jitter moves edges. Measured on the
reference view, same binary twice: interior mean abs diff **0.18/255**, ~1.2k
pixels over 4/255. Any delta below that is nothing.

**`mean |splat - svo|` is edge-dominated and is not an interior metric.** The
two backends already differ by ~24/255 at baseline, almost entirely on edges,
so a fade that only touches edges moves this number without the interior
changing at all. For interior regressions compare against the variant's *own*
`off` control and restrict to pixels eroded several steps inside the splat
footprint (`VF_SPLAT_DEBUG=1` gives a flat-coverage mask for this).

**A control that must be bit-identical, and is a real test of the plumbing.**
Render with the fade disabled and compare to the pre-change build: a
blend-enabled attachment fed `vec4(col, 1.0)` is mathematically identical to
the old opaque replace (`src*1 + dst*0`), so the off state must match to the
noise floor. It did (0.19) — which is what makes the rest of the deltas
attributable.

**Exclude water from any hole/bleed metric.** The water plane is world-wide at
`y = -0.9`, so it dominates a naive splat-vs-SVO sky comparison. Also prefer
whole-frame metrics when authoring layers have been *moved* via manifest `pos`
(per-instance geometry is then not comparable to a pre-move baseline).

Useful companion: `.opencode/wiki/concepts/splat-base-seal.md` explains the
pass structure these metrics are probing.