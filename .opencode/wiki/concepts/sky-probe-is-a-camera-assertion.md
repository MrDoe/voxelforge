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

`VF_RENDER_FLAGS=255` is the intended pre-lights escape hatch (render-flag
bits 7 and 8 off: enclosure test and detail normals). Turning **off** the
shading under test moved the number by 0.6 pp against a 6 pp shortfall, so the
failure is in the camera and the scene content rather than the lighting.

> **The off-state arm was contaminated when this was measured — re-measure the
> 0.6 pp delta.** These renders ran at 22:35; `shaders/common_base.glsl` changed
> at 23:24 and the binary was rebuilt at 23:16, and `aoShEnclosure`'s comment
> now says so outright: *"Without the bit-8 term `VF_RENDER_FLAGS=255` did NOT
> restore the pre-enclosure behaviour: this proxy kept darkening every
> shadowed+occluded surface, and a cave A/B came back identical either way."*
> So the enclosure proxy was still active in the "off" arm and
> `VF_RENDER_FLAGS=255` was **not** a bit-exact control at the time. Treat
> **+0.6 pp as provisional** until someone re-runs the pair under the gated
> build — the *conclusion* survives, the *number* does not.
>
> What does **not** depend on that A/B: the strip classification below (direct
> pixel evidence — 46.1 % warm geometry in `house`'s top eighth), and the
> time-of-day sweep further down (blue share *rising* to 56.7 % at night).
> Neither uses the escape hatch. This is the [[concepts/measurement-discipline]]
> lesson in its purest form: **an off-state control is only worth what its gate
> is worth, and "I set the flag" is not the same claim as "the feature is
> off".** Verify that the control actually disables the thing.

## The probe cannot even see the sun go down (2026-10-06)

The strongest confirmation of the claim above is not the strip classification —
it is a time-of-day sweep. Arms below are **reported by the shading session on
its tree at 640x360, hero cam `1.0 2.0 1.5 → 5.3 1.0 11.3`, splat backend**
(not measured by me alongside my 480x270 numbers above; the two sets are from
different trees and different resolutions, so compare within a column, never
across them):

| sun | mean luma | dark % | top-eighth blue % |
|---|---|---|---|
| no key / CLI 34°-238° (null control) | 120.43 | 0.01 | 32.1 % |
| elev 4, azim 240 | 100.60 | 0.03 | 46.1 % |
| **elev −30, azim 96 (night)** | 32.92 | **23.49** | **56.7 %** |

At night the blue share goes **up**, not down, so the probe still passes. The
pixel values say why — night sky `(12.5, 23.4, 37.0)`, day sky
`(127.8, 137.9, 133.9)`: `b >= r` holds in both, by 24.5 codes at night and by
6.1 at noon. `b >= r` is **brightness-independent by construction**. It tests
for *warmth*, not for *daylight*, so a 64° change in solar elevation moves the
frame by 21 pp of blue share while the assertion still reports "blue-dominant".

> **Provenance — read this before citing the table.** These arms came from the
> **CLI `--sun` flag**, not from a manifest key. `assets/world.json` is
> deliberately still **sunless** (its only keys are `layers`, `textures`,
> `version`), so every reference shot keeps elev 34 / azim 238 and its coverage
> and black-in-silhouette numbers are **unchanged** by this work. That matters
> because the opposite is the natural misreading: a reader could assume a
> `sun` key was authored and that the reference shots moved. It was not.
> The `--sun` parser itself accepts elev/azim, and `loadSunManifest` exists for
> the same values, but nothing writes the key — see
> [[concepts/sun-direction-pipeline]].
>
> The night path in `shaders/common_base.glsl` is real (`nightFactor`,
> `sunFade`, `moonDir`, `moonLight`, `applyNight`, applied to both sky
> variants), so "the shader derives the moon" in `worldfile.hpp` is accurate
> rather than aspirational. One value to not "fix": `kMoonCol` is
> `vec3(0.62,0.70,0.92) * 0.14`; the 0.62 gave mean luma 46 and read as dusk,
> so **0.14 is the settled value**.

Two conclusions, and they point opposite ways:

- The probe is not merely mis-framed on `house`/`water`; it is **structurally
  blind to the exact transition** a day/night feature introduces. A gate that
  cannot fail when the sun sets will never protect the day/night work.
- `dark %` is the number that actually moved (0.01 → 23.49). A gate keyed on a
  dark-pixel budget would have caught all three arms; one keyed on `b >= r`
  caught none of them.

The honest fix remains the one above — gate the probe on a real sky floor, or
key it on luminance rather than hue — but note that a luminance gate needs its
own reference values, because "how dark is night" is scene-dependent in a way
"is the top of the frame blue" never was.

Caveat worth recording for the next person: the null-control arm was rendered
on the shading session's tree, so it is a **null control, not a bit-identity
check** against my 480x270 numbers — different resolution, different tree.

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

## The pattern: three gates here are all calibrated on daylight

The three cases this page covers are not three bugs; they are one shape
appearing at three thresholds, and it is worth naming because it predicts where
the next false verdict will come from.

| gate | calibrated on | fails/reads-as when the regime changes |
|---|---|---|
| `sky_probe_ok` (`b >= r`, top eighth) | daylight, these 3 cameras | reads a **camera/content** change as a lighting regression |
| `black_in_obj > 0.05` | daylight | reads a **legitimate night frame** (23.49 % dark) as a rendering defect |
| ownership-mask blue `> 0.20 %` | the current layer set | reads a **new authored layer** as "terrain did not render" |

Each threshold encodes a fact about the *content it was measured against* and
is then read as a fact about *the renderer*. The failure is not sloppy maths —
the numbers are correct — it is that a content-derived constant is being used as
a universal one, and nothing in the output says which regime it was calibrated
in.

The two consequences that follow, and neither is a code detail:

1. **A day/night feature needs its own reference arms** — night shots with
   night-appropriate budgets — rather than the daylight thresholds reinterpreted
   after the fact. Otherwise every night scene fails by construction and the
   person authoring it goes looking for a lighting bug that is not there.
2. **Report the regime with the number.** A gate that prints *only* its verdict
   cannot be debugged without re-deriving the calibration. This is the same
   reason the shots print `coverage`/`black-in-silhouette`/`sky-probe` together
   instead of just PASS/FAIL — but none of them currently says "this was
   measured in daylight".

Cross-links: [[concepts/measurement-discipline]] (the general rule this is an
instance of), [[concepts/enclosed-space-lighting]] (the shading work that
surfaced it), [[concepts/splat-edge-fade-measurement]] (the other page that
leans on a bit-exact off-state control), [[concepts/focused-test-groups]]
(which group runs this script), [[concepts/interactive-ui-coverage-gap]]
(another assertion in this suite that cannot see the thing it names),
[[concepts/sun-direction-pipeline]] (the other half of the day/night split).