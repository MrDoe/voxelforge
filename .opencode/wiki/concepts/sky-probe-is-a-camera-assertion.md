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

## Known scene-content shortfall (2026-10-08, not a regression)

The `house`/`water` top strips read 0.445/0.452 against the 0.5 gate. Cause:
cabin occlusion filling the top eighth — scene content, not shading and not
coverage loss. Closed by measurement: the micro-removal contribution was
audited at 0.000% (`microDetail` is set nowhere, so zero micro surfels exist
in any gate render; exact-count units green), and the isolation flip
attributes the gap to the cabin. Do not "fix" by touching surfels, lights,
or thresholds; fix by moving the camera or the cabin.

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
it is a time-of-day sweep. Arms below are **reported by the shading session**,
on its tree, splat backend (not measured by me; not comparable to my 480x270
numbers above — different trees and resolutions, so compare within a column,
never across them):

> ⚠ **The camera attribution here is UNSOURCED (flagged 2026-10-07).** This page
> previously stated these arms were shot at *640x360, hero cam `1.0 2.0 1.5 →
> 5.3 1.0 11.3`*. **Nothing backs that line.** It was written from recollection,
> and the shading session — who produced the numbers — could not quote the
> camera either, because the scripts lived in `/tmp` and a reboot erased them.
>
> It is **unsourced, not disproved**: that tuple *is* the reference camera in
> `AGENTS.md` and the arms were plausibly run by hand, so the attribution may
> well be correct. Nobody can currently prove it, so treat it as a hypothesis,
> not a setup. I then chose my own re-measurement's camera **by reading this
> very line**, which is the real damage — the two sets are not independently
> matched and their gap is unexplained. The rule this broke: a number is not a
> gate without its command, its metric definition and its tree state — see
> [[concepts/measurement-provenance]].

| sun | mean luma | dark % | top-eighth blue % |
|---|---|---|---|
| no key / CLI 34°-238° (null control) | 120.43 | 0.01 | 32.1 % |
| elev 4, azim 240 | 100.60 | 0.03 | 46.1 % |
| **elev −30, azim 96 (night)** | 32.92 | **23.49** | **56.7 %** |

### A second, independent pair (2026-10-07) — do not merge the two

Re-measured after the day/night switch landed, through
`VF_TEST_SUN_PHASE=day|night`, which drives the **same** `App::setSunPhase()`
the Render-panel buttons and the `P` hotkey use, so it exercises the real path:

```
VF_TEST_SUN_PHASE=day|night VF_NO_OVERLAY=1 \
VF_OVERLAY_PATH=/tmp/opencode/sunphase/overlay_<phase>.vxw \
./build/voxelforge --shot <phase>.ppm \
  --cam 1.0 2.0 1.5 5.3 1.0 11.3 --width 640 --height 360
```

| arm | mean luma | dark % | top-eighth blue % |
|---|---|---|---|
| day 34/238 | 113.84 | 0.98 | 73.4 % |
| night −30/96 | 31.63 | 69.68 | 84.7 % |

Metric definitions, which the first pair never recorded and without which the
columns are not comparable: **mean luma** = mean of `(r+g+b)/3` over every pixel;
**dark %** = share with `(r+g+b)/3 < 30`; **top-eighth blue %** = share of the
top `H/8` rows with `b >= r`.

**Tree state (rule 3, and the one I nearly skipped).** Measured on the working
tree at **22:45 on 2026-10-07**, *before* the clock field landed. The clock is
write-only and leaves the default 34/238 startup path untouched, so these two arms
should still reproduce — but the tree has since moved (the clock added
`App::setSunTime`, `src/app/ui/sun_time.hpp` and a `VF_TEST_SUN_TIME` hook to the
same files), so treat the tree hash as part of the setup rather than as implicit.
Per the shading session's own settling criterion, **no `visual_check` baseline in
this tree is currently trustworthy**: all three active sessions have edited
`shaders/`, `panel_render.cpp` and `run.cpp` since his last green run, so any
coverage / black-in-silhouette figure in the suite is stale for everyone, not
just for these two rows.

**What this pair does establish.** The night/day luma *ratio* is 0.278 here
against 0.273 in the first pair — 2 % agreement, and a ratio is the one thing a
camera difference or content drift cannot fake, because both roughly cancel. The
switch moves the sun, and night shading is at the settled brightness: night came
back at 31.63, nowhere near the ~46 band that a `kMoonCol` regression to 0.62
would produce.

**What it does not.** The absolute columns disagree with the first pair
(luma ~5 %, but dark % 0.98 vs 0.01 and blue % 73.4 vs 32.1), and the blue-share
gap is far too large for content drift alone. **The overlay hypothesis for this
gap was raised here and is now REFUTED** (2026-10-07): it proposed that one pair
loaded `assets/runtime_edits.vxw` (24,636,160 B of real interactive content,
gitignored, so invisible in `git status` — see
[[concepts/overlay-silent-write-trap]]) while the other did not. But all three
sets were confirmed overlay-**suppressed** — this pair and the clock-session
curve arms ran `VF_NO_OVERLAY=1` with an isolated `VF_OVERLAY_PATH` (md5
verified unchanged), and the shading session recalls pointing
`VF_OVERLAY_PATH` at a non-existent `/tmp` scratch file, which loads nothing
either. Recollection, not an artifact — but it is the same state, so the
hypothesis is dead.

**The residual is UNEXPLAINED ON BOTH ARMS** (settled 2026-10-07). The honest
summary splits the metrics:

| metric | status |
|---|---|
| mean luma, day | **corroborated** — 113.84 vs 120.43 (~5 %), two independent setups, each measured by its own author |
| mean luma, night | **corroborated** — 31.63 vs 32.92, likewise independent |
| night/day luma **ratio** | **corroborated** — 0.278 vs 0.273, 2 %, and a ratio is what camera/content drift cannot fake |
| dark % | **disputed, unresolved** — day 0.98 vs 0.01; night 69.68 vs 23.49 |
| top-eighth blue % | **disputed, unresolved** — day 73.4 vs 32.1; night 84.7 vs 56.7 |

So: *luma corroborated across two independent setups; luminance-distribution
metrics disputed and unresolved.* Note this is **not** "the night arm settled and
only the day arm disagrees" — an earlier draft of this page leaned that way on the
hope that the night columns would converge. They did not: each author has a night
column roughly 3x the other's in dark %.

The unresolved cause is **camera and tree state** (the shading session's camera
is unsourced — see the `⚠ UNSOURCED` box above; mine is recorded verbatim with
its command). Both pairs are overlay-suppressed, so the overlay explanation is
dead (see the refutation above). Keep the sets separate and do not average them.

Do **not** re-run this as an overlay-on/off A/B — that was the one experiment the
refutation made unnecessary, and it is the only arm in this exchange whose
failure mode was data loss (a headless run with neither `VF_NO_OVERLAY` nor
`VF_OVERLAY_PATH` loads *and rewrites* `assets/runtime_edits.vxw`, having once
shrunk a session from 3.26 MB to 619 KB with no warning). The safe form, if
anyone ever does want the arm: `cp assets/runtime_edits.vxw /tmp/…`, point
`VF_OVERLAY_PATH` at the copy with **no** `VF_NO_OVERLAY`, and md5 the original
before and after.

> ⚠ **An instance of the provenance bug, in its mirror-image form — log this.**
> Mid-exchange the shading session wrote *"my settled night frame is dark% ~70
> and blue% ~85"*. **Those were the numbers from the pair in the row above, not
> its own** — which it confirmed on request. It had attached its name to another
> session's measurement while arguing *against* that measurement. This is the
> inverse of the unsourced-camera mistake, and easier to miss because it looks
> like a citation rather than an omission: an **attributed measurement** is
> measured by nobody in that form, and it is laundered by being repeated
> confidently. Ask "did you measure that, or are you quoting it?" before any
> number enters a durable record. See [[concepts/measurement-provenance]].

A genuinely matched pair — same quoted command, same camera, current tree,
metric definitions inline — is still the only thing that would settle the
residual, and it is worth doing **only** as the prerequisite for adding night
arms to `visual_check` (see the daylight-calibration table below), not to
reconcile two numbers that gate nothing.

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