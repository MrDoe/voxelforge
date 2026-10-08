---
title: "The night gate needs its own analyze(): three visual_check metrics fail on a correct night frame"
tags: [testing, night, day-night, visual-check, thresholds, calibration, gate]
sourceRefs: [tests/night_check.py, tests/visual_check.py, shaders/common_base.glsl, src/app/sun_angles.hpp, src/app/ui/sun_time.hpp, tests/test_app_cli_ui.cpp]
lastReviewed: 2026-10-08
---

> ## RE-BASELINE IN PROGRESS 2026-10-08 — do not gate against the table below
> until it lands.
>
> The flip landed the same day this band was confirmed: the light set went
> 14 → ~169 derived (store enumeration, submerged lava included) at budget
> 192. The table below is the **pre-flip record** — the day/night means,
> especially water, moved with 150+ new underwater emitters, and the ratio
> band must be re-derived against the new set before it gates again.
> Calibration done 2026-10-08 (Victor): hero 0.337, house 0.336, water 0.515
> in the 169-light state; his re-baseline write-up replaces this box.
>
> **The 0.10 … 0.27 night/day ratio band was re-measured on the tree *with* the
> 14 derived emissive lights present, and came back at the pre-emissive values,
> unchanged:**
>
> | arm | pre-emissive | with derived lights | delta |
> |---|---|---|---|
> | hero | 0.185 | **0.185** | 0.000 |
> | house | 0.143 | **0.143** | 0.000 |
> | water | 0.229 | **0.229** | 0.000 |
> | day absolutes | 123.88 / 105.41 / 111.88 | 123.88 / 105.42 / 111.90 | +0.00 / +0.01 / +0.02 |
>
> So the margin did **not** need re-deriving **pre-flip**. The earlier "stale band"
> concern — that both endpoints were measured in a tree with no derived lights —
> was a legitimate risk that the measurement closed.
>
> **Why it held, and why that generalises.** Two independent reasons, and the
> second is the one that matters:
>
> 1. The derived lights are present in **both** arms, so they largely **cancel
>    in a ratio**. A ratio of two quantities that both moved is not evidence that
>    either moved.
> 2. At the three exterior canonical cameras the emitters contribute very little
>    solid angle to a frame mean. 14 lights in a hamlet, seen from outside, moved
>    the pre-flip day mean by **+0.02/255** (re-measure at 169 — the underwater
>    set has far more solid angle in the water arm).
>
> **The corollary is the finding: this band is now empirically confirmed as
> blind.** A 20 % change in every lamp would pass it. That is not a hypothesis
> any more — it is a measured property of the gate, and it is a **demonstrated**
> argument for the proposed scale-bearing day-mean assertion rather than an
> argued one. The ratio band's robustness to content change and its blindness to
> content change are the same property: it is scale-free by design, and a
> scale-free check cannot detect a scale change.
>
> **A lights-only control is a decided "do not build".** `VF_TEXTURES=0` kills
> the derived lights but is confounded — it also swaps photo→palette albedo in
> both arms, so any delta is unattributable (found 2026-10-08: house's night
> went *up* while its day also went up, and removing light sources cannot raise
> night luma). A dedicated hook was considered and rejected: if turning the
> derived lights off moves the day arm by +0.02/255, it cannot move the night arm
> enough to matter for a ratio band. **Do not spend a hook on a 0.02/255
> signal.**

**The derived-light count is logged but not surfaced — a fourth instance of the
"correct value, present, unread" category.** The run reports a
`lighting: <authored> authored, <derived> derived from emissive materials,
<used>/<budget> slots used` line — e.g. `2 authored, 167 derived, 169/192
budget used` on the app manifest post-flip (2026-10-08) — which is exactly
the disambiguating evidence for any night number. But
`night_check` does not surface it, so the gate cannot answer the first question
a reader should ask when a night figure surprises them: *were the derived lights
even in this run?* A correct value, present, unread, in the one place a reader
would look — the same shape as the day mean being printed but unlabelled, and as
`is_sky`'s `b > 120` term. Filed in
[[concepts/measurement-discipline]]; true independent of how the control above
comes back.

# Night thresholds are not daylight thresholds

Every number `tests/visual_check.py` prints is a **daylight** number. Three of
its four break on a *correct* night render, and the one that survives cannot see
the transition. `tests/night_check.py` (group `test-night`) exists because of
this; its doc comment carries the same argument for whoever runs it.

## What breaks, verified in source and then measured

`analyze()` at `visual_check.py:69`:

```python
is_sky = b > r + 12 and g > r + 4 and b > 120
```

At the settled night sky `(12.5, 23.4, 37.0)`: `b > r+12` → 37 > 24.5 pass,
`g > r+4` → 23.4 > 16.5 pass, **`b > 120` fails**. So the sky is classified
*object*, not sky. Measured on all three canonical night shots:

| classifier | hero | house | water |
|---|---|---|---|
| daylight (`… and b > 120`) | **0.00 %** | **0.00 %** | **0.00 %** |
| same test, no `b > 120` | 40.1 % | 8.7 % | 10.0 % |

The failure direction is the counterintuitive one: because nothing is sky,
`obj_frac → ~1.00`, so **coverage fails out of range as too little sky**, not
too much. The probe being blind reads as "no sky detected"; the gate it feeds
reads as "the whole frame is object".

**`black_in_obj` is worse, and the reason is a second daylight constant.** It
counts `lum < 30` **absolutely**. The night frame's own mean is 23, so an
absolute 30 calls roughly two thirds of a correct night render "black object".
Measured shares below luma 30: **66.0 % / 86.5 % / 78.3 %**, against a 5 % gate.
This is the worst of the three — not a near miss but off by more than an order
of magnitude — and it is the one that would have been read as "the renderer is
broken".

**`ownership_classes` also fails**: it needs `g >= 100` (selected), `r >= 100`
(other object) or `b >= 90` (terrain/water). Night water is nowhere near 90, so
night matches *no* class and the ownership mask comes back empty.

## The metric that survives is the one that cannot help

`sky_probe_ok` is `b >= r` over the top eighth (`visual_check.py:78`). Night
sky `37 >= 12.5` → 100 % blue → **passes**. So the probe passes at night
precisely where the daylight gates fail hardest, which makes it useless as
evidence that the classifier above is wrong. This is the `b >= r`
brightness-independence argument from
[[concepts/sky-probe-is-a-camera-assertion]], reached from the other direction:
there it was too insensitive to catch a lighting regression, here it is too
insensitive to notice the sun set.

This is the fourth member of the daylight-calibration family on
[[concepts/sky-probe-is-a-camera-assertion]] — `sky_probe_ok`,
`black_in_obj`, the ownership-mask blue floor, and now `is_sky`/`obj_frac` —
and the general rule is the same one: **a content-derived constant read as a
universal one.**

## The gate's design, and why thresholds are ratios

`night_check.py` pins the **night preset** (`-30/96`, via
`VF_TEST_SUN_PHASE=night`), not the clock's `00:00` (`-60/0`) and not "night"
generically. The tree has three reachable nights and they are not the same
frame; `tests/test_app_cli_ui.cpp` asserts the distinction *deterministically on
the angles*, which is why it is deliberately not asserted on luma — the two
measured arms differ by 1.7 mean luma, inside the 1.3 spread that made the raw
means unusable (see [[concepts/measurement-provenance]]).

Thresholds are **ratios to the day arm at the same camera**, because absolute
mean luma is camera-dependent enough to decide this gate: the same preset and
backend measure **22.96 at the `visual_check` hero camera** and **31.63 at the
reference camera** — a 37 % gap from framing alone. Any absolute luma threshold
would be a statement about a camera, not about night.

| metric | night threshold | measured (hero/house/water) |
|---|---|---|
| night/day mean-luma ratio | 0.10 … **0.27** | 0.185 / 0.143 / 0.229 |
| night sky (own classifier) | ≥ 5 % | 40.1 / 8.7 / 10.0 % |
| top-eighth blue-dominant | ≥ 60 % | 100.0 / 75.1 / 70.9 % |
| dark share | **relative**, reported not gated | rel<0.5×: 30.8 / 39.6 / 44.9 % |

The dark share is measured two ways deliberately: **relative** (`< 0.5 ×` the
frame's own mean, the gate-scaled one) and **absolute** (`< 30`, printed as
`NOT a gate` so the daylight number stays visible without being load-bearing).

## The tripwire: VALIDATED by measurement, and set below the regression

`kMoonCol` is `vec3(0.62,0.70,0.92) * 0.14`. The `0.62` gave mean luma 46 and
read as dusk, so 0.14 is settled — and 0.62 is a *plausible-looking* number to
"brighten" a moon, which is why review will not catch it.

Measured **in-window at this camera** on 2026-10-07 (480×270 splat, hero, this
binary, this tree) — not extrapolated from another camera, and **re-measured
after the sunset transition work** rather than carried over:

| arm | night mean luma | night/day ratio |
|---|---|---|
| `kMoonCol` **0.62** (regression) | **40.06** | **0.323** |
| `kMoonCol` 0.62 — pre-sunset re-run | 40.06 | 0.323 (delta +0.13 %) |
| `kMoonCol` 0.14 (settled) | 22.96 | 0.185 |
| day arm | 123.88 | 1.000 |

The regression figure reproduces to **0.13 %** across a 222-insertion shader
change, which is what a prediction should look like when it is made from the
mechanism rather than from a number. Healthy arms top out at **0.229** (`water`).
The ceiling is set to **0.27**: between the healthy max and the measured
regression (0.041 and 0.053 of margin respectively), near the midpoint (0.276),
so the margin is symmetric rather than a guess.

**It is deliberately not 0.32.** 0.32 clears the regression by 0.003 — about 1%
margin. A tripwire that fires on a 1% margin gets switched off by the first
person it annoys, and a disabled tripwire is worse than a loose one.

Note the multiplier here is **1.74×**, not the 1.40× the reference camera's
32.9 → 46 suggested: this camera's night frame is darker and more moon-driven, so
the moon's share of it is larger. A regression measured on one camera does not
transfer to another — the reason this gate asserts ratios at a pinned camera
rather than an absolute.

**No longer provisional.** This was measured pre-sunset and flagged stale; it has
since been **re-measured post-sunset at this camera and reproduces to 0.13 %**,
so the ceiling stands on a current measurement. The mechanism argument below
explains *why* it was safe to expect that, and is worth keeping because it is
what made the prediction testable rather than a lucky repeat:

- `applyNight(d, col, …)` does `col = mix(col, nightBase, night)`, so at
  `night == 1.0` the incoming colour is **discarded wholesale** before the moon
  disc/corona/wash are added.
- `nightFactor() = 1.0 - smoothstep(-0.14, 0.06, kSunDir.y)` is **exactly** 1.0 at
  the preset's y = −0.5 — the clamp sits well clear of it.
- The sunset work adds only to `col` upstream of that, in **both** blocks —
  `skyColor` and the `skyIrradiance`/fog variant at `common_base.glsl`, which also
  ends `col *= sunDaylight(); return applyNight(...)`. The second block is the one
  that could have leaked into ground lighting; it does not.
- Day arm pinned the same way: `sunDaylight()` is exactly 1.0 at 34°.

That covers the *sky* path. `kMoonCol` also reaches geometry via `moonLight()`,
so a surface-only change could in principle hide under an identical mean; the
re-run is what closes that, at these three cameras, for this frame.

## Positive control, because a gate that never failed is not a gate

Run with the thresholds made impossible, the suite fails all three assertion
types and returns non-zero:

```
FAIL: hero: night/day mean-luma ratio 0.185 outside 0.10..0.05 (night too bright -
      is kMoonCol back at 0.62? (reads as dusk))
FAIL: hero: night sky only 40.12% (floor 99.0%) - the frame is empty, not night
FAIL: house: top eighth only 75.1% blue-dominant (floor 99.9%) - moonlight is not
      reading blue
```

So the assertions bite and the exit code propagates. That validates the
*instrument*; it does not validate the tripwire, which is the distinction
[[concepts/measurement-discipline]] keeps drawing — verify the control actually
disables or provokes the thing.

Cost is two extra world loads (one process per sun state, batched shots), so it
is a standalone group and deliberately **not** in `smoke`:

```
ninja -C build test-night
```

## Re-measured after the sunset transition fix (2026-10-07 23:55)

The sunset work added 222 insertions to `common_base.glsl` and **every threshold
figure came back bit-identical** — not approximately, digit for digit:

| metric | pre-sunset | post-sunset |
|---|---|---|
| ratio hero/house/water | 0.185 / 0.143 / 0.229 | **0.185 / 0.143 / 0.229** |
| night sky floor | 40.1 / 8.7 / 10.0 % | **40.1 / 8.7 / 10.0 %** |
| top-blue floor | 100.0 / 75.1 / 70.9 % | **100.0 / 75.1 / 70.9 %** |
| daylight classifier on night | 0.00 % all three | **0.00 % all three** |
| day arm, all metrics | 123.88 / 105.41 / 111.88 | **identical** |

Exact equality across a 222-insertion shader change is interpretable rather than
lucky: at elev −30 the sunset term contributes **exactly nothing** and at 34° it
is **exactly 1.0** (smoothstep clamps), so both endpoints are untouched by
construction and the change lives entirely in the 12° → 3° window that previously
had no transition. So the floors and the band are confirmed **on the tree the
gate will run on** and need no edits.

Cross-links: [[concepts/sky-probe-is-a-camera-assertion]] (the daylight family
this joins), [[concepts/measurement-provenance]] (why ratios and why the
camera matters), [[concepts/measurement-discipline]] (positive controls),
[[concepts/sun-direction-pipeline]] (the shading lane),
[[concepts/focused-test-groups]] (which group to run),
[[concepts/interactive-ui-coverage-gap]] (another assertion that cannot see the
thing it names).