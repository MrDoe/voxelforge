---
title: Fur raster parity — 183 ms of ray marching became 0.1 ms of quads
tags: [rendering, fur, performance, rasterization, micro-voxel, alpaca]
sourceRefs: [shaders/fur.vert, shaders/fur.frag, shaders/fur_composite.comp, shaders/common_fur.glsl, src/render/fur_pass.hpp, src/render/fur_pass.cpp, tests/fur_check.py, AGENTS.md, docs/rendering.md]
lastReviewed: 2026-10-25
---

# Fur raster parity

## The measurement that started it

"Performance was good without animation, and terrible looking at the alpaca."
Profiled with the GPU timestamp marks (`VF_TRACE=1`, 640×360, camera
`-9.5 1.8 17.0 -6.5 -0.25 18.5`, alpaca 3.6 m away):

| config | `geo` (mark 0→1) | frame avg |
|---|---|---|
| alpaca on screen, coat ON | **182.6 / 211.5 ms** | 100.0 ms |
| alpaca on screen, `VF_FUR=0` | **4.17 / 4.27 ms** | 4.52 ms |
| house view (alpaca off screen) | 5.7–6.1 ms | 6.3 ms |

So the coat was **98 % of the frame**. Not the skeleton, not the micro
sphere-trace, not the behaviour: `VF_FUR_NOANIM` changed nothing, because
the shader does the same work with identity bone matrices. The animation was
never the cost — the *pass* was.

## Why the ray march was slow

`fur_overlay.comp` walked a 4 cm bucket grid per pixel and tested **every card
in every cell the ray crossed**: a few hundred cards per pixel, and each test
fetched a 96-byte card record, fetched the bone row, did two `mat3`
multiplies, a billboard cross product and a plane intersect. Cost scaled with
the card **population** (107 055 for the alpaca), not with the pixels a card
actually covers.

Two secondary defects made it worse:

1. **The dispatch rect was the whole screen.** It projected
   `centre ± (tanHalf·aspect·z + 2 m)` — the *frustum's own* half-width plus
   2 m, not the object's extent — so every pixel the object could see
   clipped to `[0,W]×[0,H]`. The micro overlay next door already did this
   correctly (project the real 8 AABB corners); copying that pattern is the
   fix.
2. **`const FurWorld W = ft.uWorlds[w];` in every helper.** The row is
   1408 B including twelve bone rows, and `furWorldOf()` copied it once per
   loop iteration (≤16×) *per vertex*. Reading the members directly instead
   took the pass from 9.9 ms to 5.5 ms by itself.

## The raster design

One instanced quad per card, four vertices, **no vertex buffer**, one draw
call for every object (`furWorldOf` recovers the owner from the card ranges).
The quad is the ribbon's rectangle **widened by the curl amplitude**, because
the strand's centre line wanders inside the ribbon's own plane; the fragment
moves only the alpha across it.

The per-pixel lighting model is preserved instead of being re-approximated
per card: the cards accumulate six alpha-weighted terms into four additive
rgba16f targets —

```
A = (Σa, Σa·ndl·shadow, Σa·ao, Σa·mat)
B = Σa·p        C = Σa·n        D = Σa·albedo
```

— and `fur_composite.comp` evaluates `coverage = 1-exp(-Σa)`, the averaged
normal/position, `sunCol·<ndl·shadow>` + `skyIrradiance(n)·(0.28+0.30·ao)`
once per pixel, then `mix(before, alb·light, coverage)` into HDR. That is
the same model the ray version used, so the look did not move.

Depth still needs no attachment: the fragment reads `gpos` and discards a card
behind the nearest opaque surface (`fragT > sceneT + 0.01`), which is the
same rule the ray version's `tExit` used.

## Result

| | before | after |
|---|---|---|
| coat GPU time (640×360, worst camera) | 178–207 ms | **~0.1 ms** |
| `geo` on that view | 182–211 ms | **5.5 ms** |
| frame avg | 100 ms | **5.9 ms** |

The coat is now inside the noise floor of the rest of the frame (`geo` 5.50
vs 5.44 with `VF_FUR=0`).

## Gotchas worth keeping

- **Everything a card computes AVERAGES OUT at pixel scale.** A pixel sees a
  dozen cards, and the accumulation targets are alpha-weighted sums, so
  per-card variation (tone jitter), per-card relief (`detailNormal` in the
  fragment) and per-card density all collapse toward the mean. Measured: a
  ±18 % per-card tone jitter moved the gate's readability metric by 0.01, and
  dropping the card alpha 24 % moved it by −0.01. Anything that must read at
  pixel scale has to be evaluated PER PIXEL in the composite from an
  accumulated rest-frame position — which is why the pass carries five
  targets, not four.
- **`detailNormal` belongs on the coat.** It was missing, so the fur read
  hazier than the body it covers: the gate's readability metric went
  11.17 → 11.71 (needs ≥ 11.92) once the fragment sampled
  `detailNormal(restN, restPos, mId)` in the **rest** frame and re-boned the
  result. Sampling the bump in the animated frame makes the fleece crawl.
- **The min-width distance can be taken in the rest frame.** Every transform
  between the card's rest frame and the world is rigid, so
  `length(roL - root)` is the world distance — which removes a whole second
  transform chain per vertex.
- **A vision caption cannot judge a coat.** The first render described the
  alpaca as "grey, blocky"; the numeric check (warm cream at the bright end,
  the gate's blob/colour metrics) is what tells you the coat is there.

## The gate, honestly

`tests/fur_check.py` still reports two quality assertions short after the
raster rewrite (everything structural is green: the blob, the confinement, the
house/vase views, the SVO backend, the escape hatch, the turn test):

| metric | coat | bare body | required |
|---|---|---|---|
| readability (`neighbour_diff`) | 11.77 | 12.42 | ≥ body − 0.5 = 11.92 |
| texture (`luma_spread`) | 37.95 | 39.77 | ≥ body |

**The bar moved, not the coat.** The figure recorded when the coat was pinned
was a spread of **36.98 against a body of 28.82** — the coat was the more
detailed surface by a wide margin. The bare body's spread has since risen to
**39.77** (the micro body gained `detailNormal` and the finer surface), while
the coat went 36.98 → 37.95. The coat kept its texture; the thing it is
compared against gained much more.

**What does not move these numbers** (all measured, 640×360, the gate's own
camera):

| lever | readability | spread |
|---|---|---|
| baseline (density 5, alpha 0.34) | 11.71 | 38.06 |
| density 4 | 11.91 | 37.92 |
| density 3 | **12.11 (pass)** | 37.84 |
| per-card tone jitter ±18 % | 11.72 | 38.05 |
| card alpha 0.34 → 0.26 | 11.65 | 37.78 |
| whole-light AO attenuation | 11.66 | 37.81 |
| per-pixel `detailNormal` (5th target) | 11.77 | 37.95 |

The pattern is the finding: **per-card quantities average out across the
dozen cards a pixel sees**, so only a per-pixel evaluation can add detail —
and even that only buys +0.1 here, because the body's contrast comes from
geometry finer than a pixel. Closing the last 0.15 would need the coat to
stop being an alpha-weighted average (e.g. per-fragment colour blending
instead of the summed-lighting model), which is a different renderer, not a
tuning knob.

## Related

- [[concepts/fur-cards]] — what the cards are and why they exist at all.
- [[concepts/micro-animation-cpu-cost]] — the CPU side: the behaviour path was
  never the cost either.
- [[concepts/measurement-discipline]] — the profile-then-fix order this page
  follows.
