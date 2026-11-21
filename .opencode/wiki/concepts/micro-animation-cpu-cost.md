---
title: Micro animation CPU cost — what the frame actually pays for
tags: [performance, animation, micro-voxel, collision, profiling, measurement]
sourceRefs: [src/app/world/micro_objects.cpp, src/app/world/collision.cpp, src/voxel/micro_behaviour.hpp, src/voxel/chunk_store.cpp, src/app/app.hpp, tests/test_store.cpp]
lastReviewed: 2026-10-25
---

# Micro animation CPU cost

**The claim under test:** "animation takes way too much CPU power — move the
calculations to the GPU." Measured over 2400 headless frames, the *animation*
is 0.006 ms/frame of a 6.30 ms frame (0.1 %). The cost sitting around it — the
obstacle escape scan — is 0.065–0.130 ms/frame, i.e. ~90 % of the whole
animation path, and the whole path together is 1.3–2.3 % of frame time.

So the answer is **optimize the collision probing, do not move the bone maths
to the GPU.** The behaviour state machine and the pose-aware ground clamp both
need CPU-side terrain and store data; a GPU-resident animation would need a
readback to feed them, which costs more than the 0.006 ms it would save.

**Outcome:** four generic levers plus one correctness fix took the path from
0.147 to **0.0111 ms/frame** (13×) and the frame from 6.30 to 5.65 ms — and
the same instrumentation showed the path was never animation-bound at all: the
scan was re-running every frame because a defect kept it from ever concluding.
See [Results](#results-2400-frames) and
[the deadlock](#the-deadlock-the-measurement-exposed-the-real-find).

## The instrument

`App::updateMicroBehaviours` (`src/app/world/micro_objects.cpp`) carries four
cumulative buckets gated by `VF_TRACE_BEHAVIOUR`, printed every 600 frames:

```
behaviour cost over N frames: probe X ms/f (C calls),
escape-scan Y ms/f (R runs), animate+clamp Z ms/f, upload W ms/f, TOTAL ...
```

| bucket | ms/frame | notes |
|---|---|---|
| probe | 0.0084–0.0103 | exactly 1 `microFootprintBlocked` call per frame |
| escape-scan | 0.065–0.130 | 832 runs over 2400 frames (35 %; 399 in the first 600) |
| animate+clamp | 0.0055–0.0058 | `animateBones()` + the 8-corner pose-aware clamp |
| gpu-upload | 0.0024–0.0026 | `setWorldAnimation` / `setBoneRotations` |
| **TOTAL** | **0.084–0.147** | of avg frame 6.30 ms |

The counters are cumulative-over-all-frames, so the printed number falls as the
run continues (the alpaca stops being blocked once it gets clear of the rune
stones). Do not read the trend as an improvement in the code.

## Why the escape scan is 90 % of it

When the footprint reports blocked, `updateMicroBehaviours` runs a
**16-direction × 4-substep clearance scan** (`const int N = 16`,
`micro_objects.cpp`) — up to 64 further `microFootprintBlocked` calls on that
frame, each of which is `vf::voxel::cylinderBlocked` with **sides = 10,
layers = 3 = 33 sample points**, and each sample point does:

1. `worldCell(p)` → `ChunkStore::cellAt` — a *full* `StoreCell` decode
   (brick word unpack + `paletteOf` colour lookup) for a question that only
   needs `solid && obj`;
2. a loop over **every** micro field, each running the whole transform stack
   (move preview → behaviour → rotation preview → `worldOffset`) followed by
   `MicroField::sampleSdf`, an 8-tap trilinear fetch.

So one scan frame is ~2112 store queries plus ~6300 SDF samples. The measured
cost per scan run is ~0.19 ms; the single per-frame probe costs ~0.0084 ms.

`terrainTopAt` is not a cost (0.12 %) — it is one bilinear read of
`m_heightCpu`.

## Where *not* to look

- **`getenv` measured 1.01 % of cycles** in the steady-state profile, because
  `VF_TRACE_BEHAVIOUR`, `VF_FUR_NOANIM`, `VF_TEST_FUR_STAT` and `VF_TRACE_RIG`
  are looked up per frame per object. Cache them once (a function-local
  `static const bool`); `freezePoses()` already parses its spec behind a
  `static const bool once`.
- **`animateBones()` is 0.50 % inclusive** and `MicroBehaviour::update` 0.09 %.
  Sorting the leg list and the ground clamp's 8 × ~7 nearest-segment tests are
  noise.

## Measurement traps hit on the way (read before trusting a `perf` number here)

1. **World load swamps any short run.** `VoxelField::build` 37.5 %,
   `sampleWorld` 22.6 %, `smoothTerrainY` 13.5 % of a 300- and a 3000-frame
   profile. Startup is ~17.6 s, so profile with `perf record -g -D 25000` (or
   whatever exceeds the load) before drawing any conclusion about the frame
   loop. See [[concepts/load-time-field-build]].
2. **Stack unwinding is unreliable in this binary.** Frames unwind to raw
   stack addresses (`7ffc47cceb3c [unknown]`), which makes "inclusive"
   percentages contradict each other: `microFootprintBlocked` reported 4.54 %
   inclusive while **0** of the stacks containing it also contained
   `updateMicroBehaviours` — its own caller. Attribution that cannot name a
   caller is not attribution. The in-app ms/frame counters above are the
   trusted numbers.
3. **The stack *children* column disagreed with itself** for the same reason;
   do not compare `--children` totals across symbols as if they were a tree.

## Levers — implemented, all generic (no alpaca-specific constants)

The system must keep working for any micro object carrying `behaviour`, so
every lever below is per-object state or a shared query, never a name check:

1. **`ChunkStore::solidObjAt(x,y,z)`** (`chunk_store.cpp`) — an exact mirror of
   `cellAt(...).solid && cellAt(...).obj` that skips the brick colour unpack and
   the `paletteOf` lookup the probe discards. Its `Empty`/`Solid`/no-brick
   early-outs are correct *only* because nothing but `decodeCell` ever sets
   `StoreCell::obj`; a comment on the method and a doctest pin that mirror.
2. **Micro-field AABB broad-phase** — `App::microWorldBounds` computes, once
   per *probe* instead of once per sample point, which other micro fields the
   footprint cylinder can reach. Extents are the `|R|` row sums of the rest
   half-extents (the exact AABB of a rotated box, so it is conservative by
   construction). It switches off while any staged Move/Rotate preview is
   active (`microFieldSolidAt` then applies a transform the box does not model)
   and above 16 fields, where the fixed candidate array would otherwise have to
   drop one.
3. **Per-process cached `getenv`** for `VF_TRACE_BEHAVIOUR`, `VF_FUR_NOANIM`,
   `VF_TEST_FUR_STAT`, `VF_TRACE_RIG`. Two of them ran *before* their own
   `m_frameIdx == 0` gate, so a getenv per frame paid for a once-per-run
   diagnostic.
4. **Escape-scan throttle** — the chosen direction is cached on
   `MicroBehaviour` (`scanValid/scanAngle/scanClear/scanAge/scanOffset/
   scanHeading`) and reused until one of: pose moved > 0.25 m, body swung
   > 0.6 rad, arrived at the cached direction (≤ 0.20 rad), or age > 0.4 s.
   The apply step re-asserts the decision on every blocked frame, so a cached
   decision behaves exactly like a fresh one — including keeping `stateT`
   pinned in Graze.

**Rejected:** cutting `N`/substeps. 10 sides on a ~0.35 m radius is already
only ~0.215 m chord spacing against a 0.1 m lattice; that trades collision
fidelity for the last of the cost and was not needed.

## The deadlock the measurement exposed (the real find)

Instrumenting *why* the throttle was not firing turned up a pre-existing
defect, present in the baseline too. Adding an optional `int* why` reason code
to `microFootprintBlocked` (1 = water, 2 = world object cell, 3 = another micro
object) and probing the standing position as well as the stepped one gave, on
every one of the first 60 rescans:

```
rescan#4 k=0 clear=4 clears=4444444000000000 heading=-1.798
         stepWhy=2 hereBlocked=false hereWhy=0 at=(-0.025,-0.000,-0.239)
```

Read that carefully:

- **`hereBlocked=false`** — the animal is standing in *free* space;
- **`stepWhy=2`** — the step 3 mm ahead is inside a **world object cell**;
- **`clears=4444444000000000`** — the scan calls that same direction clear for
  the full 1.1 m.

The scan's ladder sampled at `step*s*0.5`, s = 1…4, so its **first rung was
0.275 m out**. It never examined the near field, so `k=0` (straight ahead) won
every time, the heading *target* equalled the *current* heading, `b.update()`
turned nothing, and the refused step was withdrawn on every frame. The offset
froze at `(-0.025, -0.000, -0.239)` and the animal sat in a limit cycle until
the state machine happened to choose Graze — which is the only thing that ever
unstuck it. **913 blocked frames and 832 scans out of 2400**, all of it spent
re-deciding a decision that could not take effect.

The fix is one line of geometry: make the ladder's **first rung the frame's own
step** (`fstep = b.speed * min(dt, 0.1f)`), so `k=0`'s verdict is *literally*
the probe's verdict. A refused step then always loses, some direction whose
near field is actually free wins, the animal turns toward it, and the next
frame's probe passes.

A coarse search ladder can bless a direction that is blocked right in front of
the agent; the search's first sample must be the step the agent will really
take next frame, or the two disagree by 0.27 m.

## Results (2400 frames, `VF_TRACE_BEHAVIOUR=1 ./build/voxelforge --smoke 2400 --width 640 --height 360`)

| | baseline | after 4 levers | + near-field rung |
|---|---|---|---|
| probe ms/f | 0.0084–0.0103 | 0.0051 | 0.0048 |
| escape-scan ms/f | 0.065–0.130 | 0.0244 | **0.0003** |
| animate+clamp ms/f | 0.0055–0.0058 | 0.0049 | 0.0047 |
| gpu-upload ms/f | 0.0024–0.0026 | 0.0015 | 0.0014 |
| **TOTAL ms/f** | **0.084–0.147** | 0.0359 | **0.0111** |
| blocked frames | 832 | 913 | **105** |
| escape scans | 832 | 832 | **5** |
| avg frame | 6.30 ms | 6.08 ms | **5.65 ms** |

**13× on the whole path**, and the animation path is now 0.18 % of the frame.
Note that the four levers alone did *not* cut the scan count — 832 rescans for
913 blocked frames — because the deadlock above re-ran the scan every blocked
frame no matter what was cached. The throttle only became visible once the
near-field rung let the animal actually escape (5 rescans for 105 blocked
frames).

The rescan-reason counters (`never/move/swing/arrive/age`) are the instrument
that proved it: before the fix they read `never 1 move 0 swing 2 arrive 830
age 1`; after, `arrive 3`. Two traps found that way, both in this page's
history: keying the "we arrived" test off `hasHeadingTarget` collapses it (the
unblocked branch clears that flag every free frame), and dropping `scanValid`
on free frames does the same thing — the three staleness tests are what expire
the cache.

## Pre-recording the movements — does baking buy anything?

Asked directly: *could the poses be pre-recorded so they need not be
calculated?* **No, and the measurements say why:**

- **The bone maths is 0.005 ms/frame** (0.08 % of a 6 ms frame). Baking poses
  removes an amount of work that was never the problem — the animation would
  still need its per-frame blend weight (gait phase, graze blend) and the
  pose-aware ground clamp, both of which need live CPU data.
- **The caches are already the "don't recompute what hasn't changed" answer.**
  The escape direction is cached; the AABB broad-phase is cached per probe;
  the getenv lookups are cached per process. What remains un-cacheable is
  exactly what *must* respond to the world: the collision probe reads the live
  `ChunkStore`, the height texture and the other micro fields, so any baked
  version would be stale the moment the user carves a wall.
- **Baking would trade a solved problem for an unsolved one**: animation state
  would have to be serialised per pose, then re-blended, while the frame clock,
  the behaviour state machine and the user's staged Move/Rotate preview all
  keep changing underneath it. That is more machinery for 0.005 ms.

The honest summary: this path was never compute-bound on *animation*. It was
compute-bound on a collision search that was running every frame because it
could not conclude — and the fix for that was one correct sample point, not a
recorded clip.

## Related

- [[concepts/collision]] — what the probe is *for*: one solidity oracle, the
  walker's circumscribed cylinder, the lift-only ground clamp, and
  `VF_TEST_COLLISION` evidence.
- [[concepts/micro-voxel-svo-overlay]] — the micro lattice, the sphere tracer
  and the fur-in-the-rest-frame rule the animation has to satisfy.
- [[concepts/fur-cards]] — the other half of "the fur follows the skeleton".
- [[concepts/measurement-discipline]] / [[concepts/measurement-provenance]] —
  why the counters beat the profiler here.
