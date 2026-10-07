---
title: Why a sunset measures as faint haze, not a sunset
tags: [shading, sky, sunset, sunrise, day-night, measurement, common-base]
sourceRefs: [shaders/common_base.glsl, src/app/cli/args.cpp, src/app/frame/run.cpp, docs/rendering.md]
lastReviewed: 2026-10-07
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

## The daylight factor must saturate, or it dims the moonlit night

The obvious fix — `mix(0.55, 1.0, smoothstep(-0.02, 0.35, kSunDir.y))` — is a
trap: at any `kSunDir.y ≤ -0.02` it settles at **0.55 and stays there**, so it
multiplies a ~45 % dimming into the entire moonlit night and silently moves
every night reference. The night is already handled by `applyNight`; a second
dimming term is not complementary, it is a regression.

"Leave the night alone" is really a constraint on *where the night preset sits*
— `y = -0.5`, i.e. the elev −30 arm, **not** `y = -0.03`. So the low edge of
the dimming window has to reach below the horizon without covering that preset:

```glsl
float daylight = 1.0 - 0.45 * smoothstep(-0.16, 0.10, kSunDir.y) *
                         (1.0 - smoothstep(0.10, 0.35, kSunDir.y));
```

- `y ≤ -0.16` (elev ≤ −9.2°, **the entire night band**): first factor is 0 →
  `daylight` is exactly **1.0**, moon untouched.
- `y ≥ 0.35` (elev ≈ 20.5°, noon): second factor is 1 → exactly **1.0**.
- `y = 0`: `smoothstep(-0.16, 0.10, 0)` = `t = 0.6154` → `0.6699`, so
  `daylight = 0.699` — the sunset is genuinely dimmer than noon.

### A correction worth keeping: low edge at exactly 0.0 is a trap

The first attempt here used `smoothstep(0.0, 0.10, y)`. At `y = 0` that is
**exactly 0**, so `daylight(0) = 1.0` and **elev 0 gets no dimming at all**.
Worse, because the dip peaks at `y = 0.10` and closes back to 1.0 by `y = 0`,
the curve is **non-monotonic** — the sky would get darker as the sun falls from
12° to 4°, then get *brighter again* at the horizon. A sunset that brightens as
it sets.

It satisfied the letter of "don't touch the night" while breaking the thing it
was written to fix. **Gate 4 caught it, because gate 4 measures elev 0** — which
is the argument for writing the gate before the formula, not after.

### These are structural claims, not measured numbers

What is established by hand and does not depend on rendering:

- `daylight(y ≤ -0.16) = 1.0` exactly → the night band cannot move.
- `daylight(0) = 0.699` exactly → the horizon is dimmer than noon.
- The curve is monotonic decreasing in `y` over `[-0.16, 0.10]`.

What is **not** established: any resulting pixel luma. Multiplying sky radiance
by 0.699 does **not** imply the frame luma falls by 30 %, because the frame goes
through AgX in post and that is not a linear transfer. Predicted lumas are
arithmetic on a false assumption and need one render to confirm. Do not quote
them.

## Gate spec — ratios, not vibes

Durable home is a new `tests/sunset_check.py` arm, since no existing reference
shot frames azim 238. Camera: `y=40`, pitched 20° up, looking toward the sun,
which puts the frame bottom at 227 m and keeps ground out of frame entirely.
**Every metric below is a ratio or a delta, so none of them is a claim about a
particular camera.**

| # | Gate | Baseline | Target |
|---|---|---|---|
| 1 | `sunsetSat = (R−B)/(R+B)`, glow band **toward** the sun, elev 2° | **0.031** | ≥ **0.16** |
| 2 | **Invariant:** high sky stays blue-dominant, `(R−B)_high < 0` | −0.33 | stays < 0 |
| 3 | **Falsifiability:** day34 anti-solar high sky must be blue-dominant, else **ABORT** | passes | must keep passing |
| 4 | **Dimming:** away-from-sun luma falls across 34→12→4→0; `luma(0) < 0.75 · luma(34)` | **0.98 — FAILS** | < 0.75 |
| 5 | **Night immovable:** `luma(−30)` within tolerance of the moonlight reference | (Robin's) | must not move |

Gate 4 is **supposed to fail today** — that is the finding, and a gate that
passes on the current tree would be measuring nothing. Gate 5 exists so a later
daylight factor cannot quietly re-dim the moon; it turns a review note into an
enforced invariant.

Gate 3 is the one that matters most for trusting any of it: two of the three
measurement runs were void because the *instrument* was broken while the script
printed a clean table. A gate that can abort is worth more than one that always
reports.

**Gate 3 must abort with a message distinguishable from a threshold failure**,
and it must do so *before* any threshold is evaluated — otherwise the first
person to hit it reads a broken render as a dim sunset and "fixes" a working
shader. Distinct exit path, distinct first line of output, no numbers printed.

Gate 4 is what caught the non-monotonic `daylight` bug above, which is the
argument for writing the gate before the formula.

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