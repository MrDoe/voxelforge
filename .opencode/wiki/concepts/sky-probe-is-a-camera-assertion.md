---
title: "The visual_check sky probe is a camera + content assertion, not a shading one"
tags: [testing, visual-check, sky-probe, camera, content, measurement, escape-hatch]
sourceRefs: [tests/visual_check.py, src/app/frame/run.cpp, assets/world.json, shaders/common_base.glsl, .opencode/wiki/concepts/enclosed-space-lighting.md]
lastReviewed: 2026-10-06
---

# `visual_check`'s "sky probe not blue-dominant" is measuring the camera

`tests/visual_check.py` prints three numbers per canonical shot and the middle
one is `sky-probe`:

```python
"sky_probe_ok": top_strip_blue / max(top_strip_n, 1) > 0.5,
```

i.e. **more than half of the pixels in the top eighth of the frame must have
`b >= r`**. With `W,H = 480,270` that is the top 33 rows, 15 840 px.

Two things make this read as a *lighting* assertion when it is not:

1. `b >= r` is not a sky test. It is "not warm-dominant". The strict sky
   classifier already exists 20 lines above it
   (`b > r + 12 and g > r + 4 and b > 120`) and is used for coverage; the probe
   deliberately uses the looser one. Between the two thresholds sits a large
   band of graded horizon/haze pixels that satisfy the probe but not
   `is_sky` (measured: 22 % of the `house` top eighth, 34 % of `water`'s).
2. The denominator is **the whole top eighth**, not the sky. So the probe
   answers "is the top of this camera's frame mostly blue", which is a
   framing question, and it will fail for any shot whose upper third is filled
   by a hillside, a roof or a tree line.

## Measured, with the shading switched off (2026-10-06)

One `--shotlist` process per configuration, `VF_NO_OVERLAY=1`,
`VF_OVERLAY_PATH=/tmp/opencode/skyprobe/…`, current tree
(`f5ab131 Reposition hamlet_tower and hamlet_boat`):

| shot | `b>=r` (default) | `b>=r` (`VF_RENDER_FLAGS=255`) | delta | strict sky | verdict |
|---|---|---|---|---|---|
| hero | 100.0 % | 100.0 % | +0.0 pp | 97.6 % | PASS |
| house | 44.0 % | 44.7 % | **+0.6 pp** | 21.7 % | FAIL |
| water | 45.2 % | 45.2 % | **+0.0 pp** | 11.2 % | FAIL |

`VF_RENDER_FLAGS=255` is the bit-exact pre-lights escape hatch (render-flag
bits 7 and 8 off: enclosure test and detail normals). Turning **off** every
shading change under test moves the number by 0.6 pp against a 6 pp shortfall.
So the failure is in the camera and the scene content, and no amount of
lighting work will turn it green.

What is actually in the top eighth (default frames):

| shot | warm/brown (wood, roof, terrain) | green (foliage/grass) | blue-ish, not strict sky | strict sky |
|---|---|---|---|---|
| hero | 0.0 % | 0.0 % | 2.4 % | 97.6 % |
| house | 46.1 % | 9.8 % | 22.3 % | 21.7 % |
| water | 41.0 % | 13.8 % | 34.0 % | 11.2 % |

`house` frames the cabin from `(2.5,1.3,6.0)` toward `(6.8,1.0,12.2)` with the
hillside behind it, so 46 % of its top eighth is warm geometry.
`water` pitches down (`4.5,-1.1,6.8`), so its top eighth is the far bank and
trees — 55 % non-sky. Both are legitimate framings; the assertion is what
assumes otherwise.

## Why the distinction cost an hour

A shading change lands, `test-visual` runs, and the report says
`house: sky probe not blue-dominant`. The obvious reading is "my ambient/
enclosure change made the sky brown". The escape hatch settles it in one
render: if the number is the same with the feature off, the feature is not the
cause. Reach for that control **before** theorising — it is the cheapest
possible refutation, and it is only cheap because the flag exists.

Corroborating the same run, `black-in-silhouette` for `house` reads
**4.29 %** default vs **2.92 %** with `VF_RENDER_FLAGS=255` (peer session
measured 4.26 % / 2.70 % — same direction, the small drift is the content
commits since). That is the metric the bit-8 work *is* allowed to move; the sky
probe is not.

## What a fix has to decide (not a code recipe)

The check has two honest readings and the project has to pick one:

- **Make it conditional**: assert blue-dominance only when the frame actually
  contains sky (e.g. gate on a strict-sky floor first), which keeps the shots
  as content references and stops a framing change from ever being reported as
  a lighting regression.
- **Re-aim the shots** so the top eighth really is sky — cheaper to read, but
  it silently edits the scene reference every future capture compares against,
  and it re-breaks the moment someone re-authors the hamlet.

Either way, the *measurement* rule is unchanged: before believing any of these
numbers, establish the same-binary noise floor and an off-state control.

Cross-links: [[concepts/measurement-discipline]] (the general rule this is an
instance of), [[concepts/enclosed-space-lighting]] (the shading work that
surfaced it), [[concepts/splat-edge-fade-measurement]] (the other page that
leans on a bit-exact off-state control), [[concepts/focused-test-groups]]
(which group runs this script), [[concepts/interactive-ui-coverage-gap]]
(another assertion in this suite that cannot see the thing it names).