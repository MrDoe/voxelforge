---
title: "The irradiance volume: emitter-lit indirect, and why it is deliberately not called GI"
tags: [lighting, indirect, irradiance, emissive, point-lights, interiors, shader, gpu]
sourceRefs: [src/voxel/irradiance_volume.hpp, src/voxel/irradiance_volume.cpp, shaders/common_irradiance.glsl, tests/test_world.cpp, shaders/common_base.glsl, shaders/common_splat.glsl, src/app/rhi/surface.cpp]
lastReviewed: 2026-10-08
---

# The indirect term that finally has an emitter in it

Until this existed, the only "indirect" light in the renderer was:

```glsl
vec3 bounce = (alb * 0.65 + vec3(0.10, 0.09, 0.07)) * fold * (0.3 + 0.7 * ao) * skyAmb;
```

That is albedo-tinted, directionless, and **sourced from nothing**. It is not
indirect light, it is a plausible-looking ambient term. Interiors read flat
precisely because no emitter appears in it.

The irradiance volume replaces it: a 64³ grid over the 102.4 m world (1.6 m
cells, 4 MB as RGBA32F, binding 26) carrying the irradiance arriving at each
cell from the light set, plus a sky-presence fraction.

## What it is not, and why the name matters

**This is not global illumination.** It is *direct* irradiance accumulated per
cell, with visibility. Light does not bounce: a hearth lights the air and walls
around it, not the far side of the room by way of the floor. One bounce is a
separate and more expensive bake.

Calling it "GI" would be a claim outrunning the evidence — the exact failure
[[concepts/measurement-discipline]] exists to catch, and the exact shape of the
defect this repo keeps paying for.

## Why a volume and not a per-fragment trace

Because the splat fragment shader already runs **twice per covered pixel** — the
opaque base pass at depth `EQUAL`, then the blended band pass — and one
per-fragment occlusion march (`softShadowSplat`) is already up to **32 dependent
texture taps**. Indirect light is low-frequency by nature; it is the one lighting
term worth a texture fetch rather than a march.

This is the same reasoning that makes the sun shadow *baked* rather than
marched — see [[concepts/dynamic-sun-shadows]] and
[[concepts/baked-sun-shadow-contract]]. On this backend, bake beats march.

## Two constraints that are easy to violate and hard to debug

**1. The emitter set is the shader's `LightUBO` slots — not a private list.**
`buildIrradianceVolume` consumes the same 256 slots `applyLights` does, authored
first then derived-from-emissive (`src/app/rhi/surface.cpp`). So the two paths
cannot disagree about which lights exist, and **truncation is shared**: when the
slots fill, both degrade together. (Landed 2026-10-08: budget 192, ~169 live —
see [[concepts/dynamic-sun-shadows]].)

That is consistent, but a dropped emitter *here* has no symptom of its own — the
frame just gets slightly darker somewhere. So the bake reports `seen` / `used` /
`cellsLit` and the caller logs them; the full seen/used line must be visible in
the log, never inferred.

**2. The attenuation must be `applyLights`' own `(1 - d/r)²`, not inverse-square.**
If the volume used physical falloff while the direct term used the clamped one,
one light would show two different falloff curves and it would read as a bug in
whichever was "right". Matching the shader's convention keeps the two terms
*additive*.

## The include-order trap (cryptic, and worth knowing before it costs an afternoon)

`common_irradiance.glsl` uses `pc.b.x` for the world size. **`pc` is declared by
the entry point** (`splat.frag`, `svo_raymarch.comp`), not by any shared header.

Included *after* the push block, it compiles clean — which is where
`common_base.glsl` sits, since `splat.frag` includes that at line 78, well after
the push declaration at line 17.

Included *before* it, glslangValidator fails with:

```
common_irradiance.glsl: 'pc' : undeclared identifier
common_irradiance.glsl: 'b' : vector swizzle selection out of range
```

The **second** error is the trap: it reads like a typo in the header rather than
an ordering mistake. And `common_surfel.glsl` is included at `splat.frag:5`,
*before* `pc` exists, so that chain can never host this header.

## A real limitation: the indirect term's visibility is COARSER than the direct term's

The bake's visibility march uses the height texture plus the coarse object
volume — deliberately, so it agrees with the splat path's
`lightVisibilitySplat`. That march **steps over geometry thinner than its own
sampling resolution**: `objDist` is a 0.4 m volume clamped to ±1.26 m, so the
sphere-trace step is capped near 1 m, and a thin wall gets stepped over. Light
**leaks through thin geometry**.

This was not assumed — it was found by a test that asserted irradiance falls off
monotonically along a ray, and failed with six violations. The leaked light
lands on surface the camera may not be looking at, so **no image comparison
would ever have shown it.**

It is a fidelity gap, not a bug, and it is not fixed here: the exact lattice
would mean the 3.2 s `VoxelField` EDT, which is precisely the cost this volume
exists to avoid. The consequence to keep in mind:

> The volume's **indirect** shadows have a coarser visibility model than the
> sun's **direct** shadows, which are baked against the exact 10 cm lattice. In a
> lit room, a lamp's shadow can be softer or leakier than the sun shadow beside
> it. That is expected — do not "fix" it without understanding the trade.

### The invariant that actually holds, and is worth reusing

The monotonicity assertion was replaced by the physically correct bound:

> Occlusion can only **reduce** irradiance, so every cell must satisfy
> `volume ≤ (1 − d/r)²` at its own distance from the emitter.

That is stronger than monotonicity, because it pins the *normalisation* rather
than the ordering, and it holds regardless of leaks. It is paired with a
whole-volume bound: for a `(1-d/r)²` emitter the free-space volume integral is
`π·r³/3`, and the baked total must not exceed it.

Both live in `tests/test_world.cpp` under `irradiance volume:` and both assert
their own coverage (`rays > 0`, `checked > 20`) — a bound that walks zero cells
passes vacuously, which is the failure mode this page keeps running into.

## The bug a test caught and an eye would not

A cell **containing** an emitter came out **unlit**. The bake skipped
`dist <= 1e-4f` to avoid normalizing a zero-length direction — but `(1-d/r)²` is
1.0 at `d = 0`, so the emitter's brightest cell was a black hole while every
neighbour lit correctly.

Fixed by short-circuiting visibility to `1.0` at zero distance instead of
skipping the contribution.

**It was found by the test, not by looking**, and it would not have been visible
in a frame mean either — see the next section. The test lives in
`tests/test_world.cpp` under `irradiance volume:` and is deliberately
**camera-free and GPU-free**.

## The verification problem: frame means cannot see this feature

This is the part that generalises, and it was measured rather than assumed.

Landing the **14 emissive-derived lights** moved the day arm's frame mean by
**+0.00 / +0.01 / +0.02** out of ~110–124 — about **0.02%**:

| camera | day mean before | after | delta |
|---|---:|---:|---:|
| hero | 123.88 | 123.88 | +0.00 |
| house | 105.41 | 105.42 | +0.01 |
| water | 111.88 | 111.90 | +0.02 |

So a frame mean is effectively **blind to a localised lighting change** — and the
irradiance volume's entire purpose is a localised change. Verifying it with a
frame mean would return "no effect" and be wrong in the *opposite* direction from
a false positive.

Consequences, both durable:

- The night band is **valid but nearly vacuous with respect to lights**: a 20%
  change in every lamp would pass it. That is a fact about global means, not a
  defect to fix in the band.
- A **lights-only** control (`VF_EMISSIVE_LIGHTS=0`-style) is *not* worth
  building: if turning the derived lights off barely moves the day arm, it cannot
  move the night arm enough to matter for a ratio band. Build it when a real
  unresolved question needs it, not speculatively against a 0.02/255 signal.
- The instrument with power is a **local interior-region mean**. Expected
  signature: the local mean rises while global means stay put. If the global
  means move instead, that is **leakage** and a real failure.

This is the same blind spot as [[concepts/splat-edge-fade-measurement]] ("never
use `mean |splat−SVO|` as an interior metric") and the same finding as
[[concepts/tile-splat-parity]] — global statistics are edge- and average-dominated,
so they cannot see a change confined to a region.

## Build cost

The bake's work is `64³ × ~171 lights ≈ 44.8M` (cell, light) pairs, which is
**~10×** the 4,191,884 surfels the existing bake already handles — and that bake
is measured at 2.1 s warm / 2.9 s cold. So the volume should land at a few
seconds single-threaded, i.e. **~0.5–1 s across 8 threads**, inside the ~7.7 s world
load.

That is an **order-of-magnitude estimate from a neighbouring measurement**, not a
measurement of this kernel, and it is labelled as such. The object field is a
sparse hash, so per-sample cost could be several times either way.

It is meant to rebuild on **world/atlas reload only**, so it adds nothing to the
7.92 s sun-change latency in [[concepts/dynamic-sun-shadows]].

**That property is conditional, and the condition is not about placement.** The
natural call site is `applyWorldReload()`, right after `uploadLightSources()`
(same emitter set, no stale copy) — but `applyWorldReload()` **is** the
sun-change path today, because `requestWorldReload → m_layers.requestReload →
consumeRebuild → applyWorldReload`. So until the re-bake shortcut removes a sun
change from that path, a bake at the natural call site costs ~0.2–0.4 s on
**every** sun change: small, silent, and it reads as "the volume updated" rather
than as a regression.

Once the shortcut lands, a sun change no longer reaches `applyWorldReload` at
all and the exclusion is automatic. Excluding it earlier would need an explicit
content-dirty flag — more state than it is worth.

> **Two figures here need reconciling with the rest of the wiki (flagged 2026-10-08,
> not yet resolved).**
>
> 1. **"~7.7 s world load"** — every other page quotes **~17.6 s** for the full
>    world load ([[concepts/load-time-field-build]], and the 13.2 s EDT inside
>    it). 7.7 s may be a warm-reload or a partial-load figure, but as written it
>    contradicts the canonical number without saying so. If it is a warm reload,
>    say "warm reload"; if it is the full load, it is wrong.
> 2. **"7.92 s sun-change latency"** — this one *is* consistent with
>    [[concepts/dynamic-sun-shadows]] (which quotes the same 7.92 s), but note
>    that the sun-change latency and the full world load are **different
>    measurements** and the page uses both in adjacent sentences. A reader can
>    easily conflate them.
>
> **Binding 26 is new, and the descriptor pool must grow for it.** The
> `LightUBO` at binding 25 already taught this lesson: a new binding at the end
> means growing the binding array **and** the descriptor count, because the array
> is sized to the highest index + 1, not to how many entries are filled. The
> same applies at 26 — and the tile set's binding array is indexed by *binding
> number*, so it needs the same treatment. See
> [[concepts/enclosed-space-lighting]] for the trap that made the first lamp test
> a silent no-op.

## The loop visited a quarter of the volume, and the app read it as an occlusion failure

`kSlice = 4` is the **number of slices**, but the loop used it as the z stride
(`sz * kSlice`), covering z in [0,16) of 64 — one quarter, at the world edge,
while 15 of 16 emissive clusters sit near z +7…+15. Every (cell,emitter) pair
missed the radius test before visibility ran, so the app logged "14 emitters
seen / 0 used / 0 cells lit". Fixed to `kZPerSlice = kN / kSlice`: 8 of 14 used,
13 cells lit, max RGB 0.987 (app's `assets/world.json`, CPU-only).

Neither live hypothesis predicted this — one said resolution, the other
visibility; both reasoned about physics while the defect was in iteration
bounds. What found it was a census **inside** the bake disagreeing with an
independent caller's table about the same light set (40 pairs in range vs 0
visited). Two instruments, one codebase, one light set: the disagreement was
the signal.

The six existing cases passed with the bug present (6/6, 153 assertions, fixed
and reverted alike): they place a synthetic emitter wherever `findOpenAirCell`
lands, and the truncated band contained that spot. The seventh case,
`irradiance volume: an emitter lights cells at ANY world height`, pins six
emitters at fixed world z from −30 to +40 (measured: ~220 cells lit each when
fixed, 0 outside the band when reverted) plus a structural sky-channel coverage
count that needs a real emitter — the empty-emitter early return zeroes the
volume before the loop, so "no emitters" and "visited nothing" are identical
outputs. Its own counter first shipped counting (z,y) rows (4096) instead of
slices (64), because `break` exits only the inner loop; now a per-slice
found-flag.

## Status

| pieces | state |
|---|---|
| `src/voxel/irradiance_volume.{hpp,cpp}` | landed, compiles in the real build |
| `shaders/common_irradiance.glsl` | landed, verified to compile in-order |
| `CMakeLists.txt` (source + shader `DEPENDS`) | landed |
| tests (`irradiance volume:` × 7) | landed, **180 assertions**, `test-world` 2/2 green via ninja |
| `VF_NO_IRR_VOLUME=1` escape hatch | landed. Read as the shading side: exact zeros uploaded → `irradianceVolume()` returns exactly 0.0 → the ternary takes the **old-stand-in** branch, kept byte-for-byte. So it is bit-exact against *a build that never had the feature*, conditional on two things in **his** code: the upload always runs (even all-zero), and the fallback expression is never tidied. Note it is indistinguishable by design from a scene with **no emitters** — both mean "the volume has nothing to say" |
| descriptor / upload / shading integration | **in progress** — the shading session owns those files; shader side landed (include + twin-text with an old-stand-in fallback) |
| first baked frame | **not yet** — no rendered frame has shown this volume |

## The world is ORIGIN-CENTRED, and the bake was 51.2 m off for one commit

**This is the most important thing on the page, because it shipped green.**

The first version of `buildIrradianceVolume` put its cell centres at
`(i + 0.5) * kCell` — that is `0 .. WORLD`. But voxelforge's world is
**origin-centred**, `-WORLD/2 .. +WORLD/2`, so the shader read every cell
`WORLD * 0.5 = 51.2 m = 32 cells` from where the CPU had evaluated it.

Three independent authorities, any one of which settles it:

| authority | says |
|---|---|
| `VoxelField::sampleWorld` (`voxel_field.cpp:804`) | `int((p.x + 0.5f * WORLD) / VOXEL)` — centred |
| the shaders, for `uObjVol` and `uIrrVol` | `clamp(p / pc.b.x + 0.5)` — centred |
| `heightmap.hpp` | `kHmMinMeters = -8.0f` — the authored terrain starts at a **negative** metre, so world coordinates are routinely negative and a 0-based world cannot contain it |

`--probe` corroborates: `probe(0,0,0)` is air just above the hamlet surface,
`probe(51,0,51)` is solid 11.85 m in. The hamlet and the reference camera
(`1.0,2.0,1.5 → 5.3,1.0,11.3`) sit near the **origin**, not in a corner.

### Why the symptom was worse than an offset

Not a shift — an **inversion**. A sealed interior read the cell the CPU had
evaluated for open sky 51.2 m above it, so the world-wide `w` sky channel was
`sunUp` indoors: interiors washed with sky *and* denied the lamp light they were
supposed to be showing. Two opposite failures in one term, which is why any test
that checked either symptom in isolation would have found something that looked
right.

### Why 232 passing assertions did not notice

`tests/test_world.cpp`'s `irrCellCentre()` reproduced the same 0-based formula
as the bake. Bake and test agreed perfectly — a closed loop with no external
reference point, so **the assertion count was never evidence about the frame at
all**. 232 assertions is not stronger than 5; it is one check counted 232 times,
equally happy reporting the wrong frame.

The general form of this is on
[[concepts/measurement-discipline]] (Wendy's page, instance nine); it is not
restated here. What belongs here is the shape the bug actually had in the code:

- the **forward** direction (cell → world) had exactly **one** call site, so it
  was reviewable;
- the **inverse** direction (world → cell) had **two**, both spelled inline as
  `int(air.x / c)`, both 0-based — so the emitter sat in one cell while the test
  read another.

That asymmetry is the bug's real geometry, and it is why the fix is *one helper
per direction* (`irrCellCentre` forward, `irrCellIndex` inverse) rather than
"remember the offset". With the bake fixed but the test still 0-based,
`stats.used` came back **0** — the emitter reached no cell centre at all.

### The assertion count went DOWN, and that is the tell

| | cases | assertions | ray walk | rays |
|---|---:|---:|---:|---:|
| broken frame | 5 | **232** | 186 cells | 6 |
| fixed frame | 6 | **152** | 89 cells | 3 |

A correct fix that removes assertions should prompt a question, not a
congratulation. Measured cause: with the emitter in the wrong half of the world
its 6 axis rays travelled much further through open space before hitting
anything, so it performed **more** work and asserted about **97 cells unrelated
to the light**. The green suite was busy.

Both numbers were read by *forcing the gate to fail* — `CHECK(checked > 1e6)` —
because a passing `checked > 20` never says whether `checked` was 20 or 200. That
is the cheapest technique in this page and it is worth stealing: any coverage
gate should be readable as a number without editing the test.

Free-space energy after the fix: `total 585.06` against the `π·r³/3` ceiling of
`2873.51`, **ratio 0.204** — occlusion removes ~80 %, as it should.

### Defence

The test file now carries a case, `irradiance volume: the cell frame is
origin-centred`, whose only job is to break that closed loop: it checks
`IrradianceVolume::kOriginOffset` against `VoxelField::sampleWorld`'s own
conversion, written out longhand so it is a genuinely second implementation that
can disagree. It is the one place the chain meets something external.

## Round, do not truncate — 8 of 64 cells were silently the wrong cell

The frame helpers are deliberately *independent* expressions, forward
`(i+0.5)·c − o` and inverse `(p+o)/c − 0.5`. Independence is the point — it is
what lets them disagree — but a subtraction and a divide between them do **not**
compose exactly in float32: the result lands within `3.7e-8` of an integer, so
`int()` **truncation** sent cells to the *previous* cell.

Measured by reverting the fix and letting the gate name them — **8 of 64**:
`3, 5, 8, 10, 23, 44, 49, 54`. The fix is `std::lround`, which moves the
decision threshold from "must be exactly 0" to "may be 0.5 off" — the margin a
subtract-divide-truncate should have had.

The test now loops the round-trip over every cell rather than spot-checking, and
that loop is what proves the fix: reverted, it reports all eight by name.

> **A model of an instrument is not the instrument.** A Python model of this
> expression predicted **4** failing cells (3, 5, 10, 11). The compiled
> expression fails on **8**. Same quantity, twice the cells — the C++ and NumPy
> rounding disagree in a way I did not look for. The page carries the 8, from
> the compiled gate. This is the stale-binary lesson again, one level down: I had
> spent the day insisting that a measurement must come from the instrument, then
> quoted a model as if it were one.

## The upload contract, enforced rather than documented

The shading side's upload does **no packing** — it hands `cells.data()` to
`vkCmdCopyBufferToImage` as a tightly-packed `RGBA32F` image. That is the right
choice for exactly the reason this page keeps tripping over: a pack step would be
a *second* expression that can disagree with the first, and today there is
nothing to assert against.

Verified here rather than assumed: the bake writes
`out.cells[(z·n + y)·n + x]` — **x fastest, then y, then z** — which is
Vulkan's tightly-packed order, and it matches `chunkIndexOf`'s canonical z-major
convention used by the SVO/surfel/store paths. `sizeof(glm::vec4)` is **16** with
member offsets 0/4/8/12, so the byte-copy is legal.

But *legal by convention* is not *enforced*, so `irradiance_volume.hpp` now
carries the contract as code:

- `IrradianceVolume::indexOf(x, y, z)` — the linear order, defined **once** on
  the CPU side; the bake writes through it;
- `kBytes` — the true upload size, with a `static_assert` tying it to
  `kN³ · sizeof(glm::vec4)`;
- `static_assert(sizeof(glm::vec4) == 16)` — changing the cell type to something
  with padding is now a **build error**, not a silently misaligned 2 MB upload.

The same index must agree in three places: the bake's write, the test's read, and
the shader's texel fetch.

**The test keeps its own independent `irrIndex` on purpose.** Collapsing it into
`indexOf` would remove the one consumer outside the pair that *can* disagree —
which is precisely the capability whose absence let the frame bug through 232
assertions. A deliberate duplicate is cheaper than a closed loop.

## The CPU visibility march must MIRROR the GLSL — and no test can catch it drifting

`visibilityTo()` is a hand port of `lightVisibilitySplat`
(`shaders/common_splat.glsl`). It has to stay identical, including the
`max(distance, VOXEL * 0.35)` floor before the `0.85` scale.

An early version of the port folded that floor away, reasoning that the following
`clamp(…, 0.05, 1.2)` made it redundant. **It is redundant today** — for any
distance under ~0.04 m both forms land on the 0.05 floor — and that is exactly
why it was dangerous: the two agree only by coincidence of constants. Editing
`VOXEL` or the `0.85` would silently make the CPU march disagree with the GPU one
it exists to mirror, in the direction of *fewer and longer steps*, which leaks
**more** light through thin geometry.

It compiled. It passed all 232 assertions. No lint rule saw it. Only reading the
two implementations side by side did. If you change one, change the other, and
diff them rather than trusting the tests.

The same reasoning is why the CPU path reuses `applyLights`' `(1-d/r)²` instead of
a physically-derived falloff: two implementations of one convention must not be
allowed to drift, and the shader's convention is the one the product ships.

Everything above is verified **at the data level** — the bake compiles in the
real build and its contract is pinned by 224 camera-free assertions. Nothing is
verified on screen yet, so this page must not be read as claiming a visual
result. The measurement that will settle it is a **local interior-region mean**,
not a frame mean, for the reason given above.

## Cross-links

[[concepts/enclosed-space-lighting]] (the lights lane this feeds, and the
`aoShEnclosure` guard that keeps cave daylight off),
[[concepts/night-gate-thresholds]] (the ratio band this is invisible to),
[[concepts/dynamic-sun-shadows]] (the same bake-don't-march reasoning),
[[concepts/measurement-discipline]] (why the frame-mean control was rejected and
the camera-free test used instead),
[[concepts/overlay-silent-write-trap]] (any render run needs `VF_OVERLAY_PATH`).
