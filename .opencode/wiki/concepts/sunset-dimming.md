---
title: Why a sunset measures as faint haze, not a sunset
tags: [shading, sky, sunset, sunrise, day-night, measurement, common-base]
sourceRefs: [shaders/common_base.glsl, src/app/cli/args.cpp, src/app/frame/run.cpp, docs/rendering.md]
lastReviewed: 2026-10-07
numbers: |
  Measured by this session (baseline, before the fix): toward-sun horizon
  saturation 0.031 and R-B +12.6 at elev 2; away-from-sun frame luma 164 at
  elev 34 vs 172 at elev 4; high sky R-B -38.
  Measured by George (after the fix, 480x270 splat, same camera): toward-sun
  saturation 0.120/0.143/0.160/0.168/0.179/0.192 at elev 12/8/4/2/0/-2 and
  R-B +63.3 at elev 2; away-from-sun frame luma 155.25/151.41/143.90/133.50/
  110.43/89.92 at elev 34/12/8/4/0/-2, ratio 0.711; high sky R-B all negative,
  worst -12.5.
---

# The golden-hour gradient exists — it is just ~3× too weak to read

Measured 2026-10-07 at 320×180, camera at `y=40` pitched 20° up so the frame
bottom (−10°) meets ground at `40/tan(10°) ≈ 227 m`, outside the 102 m world —
**every sampled pixel is sky**. Sun azimuth 238 throughout. Values are
display-referred post-AgX, so they are what the eye actually receives.

**Away from the sun** (the general sky), glow band just above the horizon:

| elev | R,G,B | luma |
|---|---|---|
| 34 (day) | 155.9, 163.9, 174.5 | 164 |
| 12 | 169.5, 169.8, 174.8 | 170 |
| 4 | 172.5, 171.1, 174.8 | 172 |
| 0 | 160.5, 159.4, 164.0 | 161 |
| −2 | 141.4, 141.0, 147.2 | 141 |

**The sky does not dim through golden hour.** Luma goes 164 → 172 → 161 → 141: it
is *as bright at elev 4 as at noon*, merely desaturating to neutral grey. Warm
share (r > b) is **0 % at every elevation** — the anti-solar sky never warms.

**Toward the sun** (the glow), same band:

| elev | R,G,B | R−B |
|---|---|---|
| 20 | 189.3, 186.5, 189.7 | −0.3 |
| 12 | 204.0, 194.9, 194.6 | +9.3 |
| 8 | 208.2, 197.6, 196.3 | +11.9 |
| 4 | 209.1, 198.2, 196.6 | +12.5 |
| 2 | 206.5, 195.3, 193.9 | **+12.6** |
| −2 | 174.0, 164.2, 164.4 | +9.6 |

There **is** a warm horizon toward the sun, peaking around elev 2–8. So the
gradient exists and its **sign is right**: warm at the horizon, blue above.

The problem is magnitude. Peak `R−B` is **+12.6 / 255** while the high sky
directly above it is `R−B = −38`. A warm smudge a third the strength of the blue
above it reads as haze on a blue sky, not as a sunset. A convincing one wants
peak horizon `R−B` around **+60…90** with the high sky still blue.

## Three causes, all in `skyColor`/`skyColorFast`

1. **`base` never scales with sun elevation.**
   `mix(kHorizon * 1.05, kZenith * 0.95, …)` uses fixed constants, so the
   luminance floor is identical at every sun angle. Nothing dims for golden
   hour — hence the flat 164 → 172 luma above.
2. **`horizBand = pow(1.0 - cosTheta, 3.0)` is cubic**, confining the tint to a
   thin strip near `d.y = 0`.
3. **`applyNight` starts at `kSunDir.y = 0.06`, i.e. elev ≈ 3.4°.** So the whole
   12° → 3° window — where a real sunset actually happens — has *no* transition
   in it. The only low-sun change in that window is `lowSun`, which is an
   additive tint, not a dimming.

Proposed (not landed at time of writing — George's lane, see
[[concepts/sun-direction-pipeline]]): a daylight factor on `base`, a wider
`horizBand`, a stronger `sunsetTint`, and a dusk ramp separated from
`applyNight` so 12° → 3° is a transition rather than a step.

## What landed (supersedes the proposals below)

**Read `common_base.glsl` for the current form; the two earlier proposals on
this page are both dead.** What is in the tree:

```glsl
float sunDaylight()
{
    return mix(0.36, 1.0, smoothstep(-0.14, 0.35, kSunDir.y));
}
```

- **Monotonically increasing in `y`**, so the sky dims monotonically as the sun
  sinks with **no recovery before the horizon**. An earlier two-sided bump
  peaked at 5.7° and opened back to 1.0 at `y = 0`, which would have made the
  sky *brighten* as it set.
- **The moonlit night is protected structurally, not by a saturating window.**
  The ramp is allowed to keep falling below the horizon because `applyNight`
  **replaces the sky outright** once `nightFactor()` reaches 1 (`y ≤ -0.14`) —
  so for the entire night band this factor has *no effect at all*. That is what
  frees the ramp to be monotonic instead of squeezing a recovery into the
  twilight band. The earlier proposal here added a two-sided smoothstep purely
  to protect the night; once the replacement semantics are noticed, that
  machinery is unnecessary.
- **The floor is 0.36, not 0.50.** AgX compresses hard: a scene-space 0.60 at
  the horizon measured **0.87 of the day value on screen**, against a required
  0.75. The floor has to be chosen in **output** space and then converted,
  which is why it looks deeper than the curve suggests. *Anyone tidying 0.36 up
  to 0.50 because it looks too dark will silently fail gate 4.*

Also in `skyColor`/`skyColorFast`: `horizBand` widened from
`pow(1 - cosTheta, 3.0)` to `1.9`; `sunsetTint` made directional and far more
saturated, `(1.0, 0.47, 0.12)` toward `(1.0, 0.34, 0.26)` away; and the
**anti-solar floors** rebalanced — the golden-hour terms were `0.55` and `0.35`,
which lit the *whole horizon ring regardless of where the sun was* and made the
away-from-sun sky brighter than the daylight dimming removed (away luma rose
155 → 160 from 34° to 12°). They are now **0.20 and 0.15**, with the toward-sun
coefficients raised so the sums stay at 1.95 / 1.00 — the toward-sun glow is
unchanged by the rebalance. **That rebalance, not the daylight curve, is the
mechanism behind gate 4 passing.**

## Dead proposal 1 — the fixed daylight factor

`mix(0.55, 1.0, smoothstep(-0.02, 0.35, kSunDir.y))` is a trap: at any
`kSunDir.y ≤ -0.02` it settles at **0.55 and stays there**, multiplying a ~45 %
dimming into the entire moonlit night and silently moving every night
reference. Kept here only so it is not re-proposed.

## Dead proposal 2 — the saturating two-sided bump

```glsl
float daylight = 1.0 - 0.38 * smoothstep(0.0, 0.10, kSunDir.y) *
                         (1.0 - smoothstep(0.10, 0.35, kSunDir.y));
```

Written to keep `daylight = 1.0` outside the twilight window. It failed **its own
gate 4**: `smoothstep(0.0, 0.10, 0)` is *exactly* 0, so `daylight(0) = 1.0` and
elev 0 — the elevation gate 4 measures — got no dimming at all. It satisfied the
letter of "don't touch the night" while breaking the thing it was written to
fix. **Gate 4 caught it because gate 4 measures elev 0**, which is the argument
for writing the gate before the formula. Superseded: a monotonic ramp plus
`applyNight`'s replace semantics is both simpler and correct.

## Measured after the fix

Setup verbatim: eye `(0,40,0)` pitched 20° up, 480×270 splat, horizon band rows
0.72–0.95, high sky rows 0.05–0.25, sun azim 238. "Toward" aims the camera at
the sun; "away" aims at azim 58 with the sun still at 238.

**Toward sun — saturation `(R−B)/(R+B)`, horizon band:**

| elev | 34 | 12 | 8 | 4 | 2 | 0 | −2 |
|---|---|---|---|---|---|---|---|
| sat | −0.037 | 0.120 | 0.143 | 0.160 | 0.168 | 0.179 | 0.192 |

`R−B` at elev 2 is **+63.3** (baseline +12.6), clearing the +60 target. The
high-sky value that was thinnest (toward, 8°) is now −12.5, was −2.4.

**Away from sun — frame luma, the monotonicity gate:**

| elev | 34 | 12 | 8 | 4 | 0 | −2 |
|---|---|---|---|---|---|---|
| luma | 155.25 | 151.41 | 143.90 | 133.50 | 110.43 | 89.92 |

Strictly decreasing; ratio **0.711 < 0.75**.

**High sky stays blue-dominant** (`R−B < 0`): away −48.2 / −43.2 / −42.3 /
−41.8 / −41.4; toward −29.9 / −13.7 / −12.5 / −14.0 / −15.5 / −17.7 / −21.1.

## Gate spec — ratios, not vibes

Durable home is a new `tests/sunset_check.py` arm, since no existing reference
shot frames azim 238. Camera: `y=40`, pitched 20° up, looking toward the sun,
which puts the frame bottom at 227 m and keeps ground out of frame entirely.
**Every metric below is a ratio or a delta, so none of them is a claim about a
particular camera.**

| # | Gate | Metric | Baseline | Threshold | Measured |
|---|---|---|---|---|---|
| 1 | Sunset reads as a sunset | `sunsetSat = (R−B)/(R+B)`, **horizon band**, toward sun, elev 2° | 0.031 | ≥ 0.16 | **0.168** |
| 2 | High sky stays blue | `R−B < 0`, high sky | −0.33 | < 0 | **−12.5** (worst case) |
| 3 | Probe is valid | day34 away-sun high sky blue-dominant, else **ABORT** | passes | must pass | passes |
| 4 | Golden hour gets darker | `luma(0) / luma(34)`, **away from sun**, **frame luma** | 0.98 | < 0.75 | **0.711** |
| 5 | Night must not move | `luma(−30)` vs the moonlight reference | — | unchanged | see `tests/night_check.py` |

### Each gate uses the metric that matches what it measures

**Do not harmonise these into one metric.** Gate 1 and gate 4 both *could* be
computed from the horizon band, and the band is the worse choice for gate 4:
measured on the band the ratio is **128.3/170.7 = 0.752**, which fails 0.75 by
0.002 — and that 0.002 is the band talking, not a real violation. The band is a
narrow strip whose **content** moves with elevation (cloud cover is
`0.46 + 0.10·(1 − smoothstep(0, 0.6, kSunDir.y))`, so which pixels are cloud
versus clear sky changes as the sun sets), and it sits in the AgX shoulder where
the glow lives, so a small radiance change moves the encoded mean a lot.

Gate 1 wants the band because the glow lives there. Gate 4 wants frame luma
because at `y = 40` pitched 20° up the frame is **all sky by construction**, so
frame luma *is* sky luma and nothing is contaminated. Collapsing them would
weaken one gate to spare the other.

Thresholds are written as the **measured** value with the setup attached, not
as a rounded target.

**Gate 3 must abort with a message distinguishable from a threshold failure**,
and it must do so *before* any threshold is evaluated — otherwise the first
person to hit it reads a broken render as a dim sunset and "fixes" a working
shader. Distinct exit path, distinct first line, no numbers printed.

## No reference shot frames a sunset

The hero camera looks toward `+x,+z`; the sun at azimuth 238 is toward
`−x,−z`. **`visual_check` never sees a sunset**, so the gate stays green no
matter how good or bad this is. Any sunset change needs its own reference arm
before it can be claimed — the same class of gap as
[[concepts/sky-probe-is-a-camera-assertion]], one step further out.

## Probe hygiene — this measurement took three attempts

Two of the three runs were void, and both failures were in the instrument:

- **A partial `--cam` token silently discards the target.** `--cam` accepts
  either six argv tokens or one six-value comma token; passing three values
  fills only `camx/camy/camz` and leaves `tx/ty/tz` at defaults. Both "toward
  sun" and "away from sun" rendered **byte-identical**, pointed at terrain. The
  tell: two viewpoints that must differ produced *every row equal*.
- **`describe_image` mislabelled the frame** — "top-down aerial view… with a
  blue river" for a perspective render. Do not use a caption to judge a sky.

What made run 3 trustworthy: camera height chosen so no ground can enter the
frame regardless of angle error, plus a **falsifiability gate** — day34's
anti-solar sky must be blue, or the script exits non-zero instead of printing a
clean table. A probe that can fail is worth more than one that always reports.

Cross-links: [[concepts/sun-direction-pipeline]], [[concepts/shading-model]],
[[concepts/measurement-provenance]], [[concepts/sky-probe-is-a-camera-assertion]].