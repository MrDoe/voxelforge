---
title: Localising a run-to-run render difference
tags: [rendering, determinism, vulkan, frames-in-flight, debugging, micro, splat]
sourceRefs: [shaders/micro_mesh.frag, src/render/micro_svo_pass.cpp, src/render/micro_svo_pass.hpp, src/app/world/micro_objects.cpp, src/app/frame/record_headless.cpp, src/app/frame/record_interactive.cpp, src/app/frame/record_studio.cpp, tests/fur_check.py]
lastReviewed: 2026-11-13
---

Two runs of the same binary, camera and pinned pose MUST produce the same
bytes. Headless `--shot` never advances `m_animTime`, so wind, grass and water
are pinned too: a duplicate-pose pair is a **determinism test with an expected
floor of zero**, not a tolerance. `tests/fur_check.py`'s "noise floor" is
therefore a bug detector, and the `<= 200` bound is a legacy ceiling, not a
budget.

## The bisect ladder (cheapest cut first)

> **VERDICT IN FLIGHT (2026-11-13).** The mesh loader now has a
> winding-orientation pass and the pipeline can cull back faces; a peer
> session's verification renders are running against it. Everything below
> describes the state BEFORE that change, so read the "quantitatively
> unexplained" and "eligibility" sections as the pre-repair measurement, not as
> the final verdict. Two facts from the cull fork are already reported and are
> consistent with it: the residual after culling is near-disjoint from the
> baseline (Jaccard 0.002), which is what an orientation-owned cause predicts.
> Update this page with the landed numbers when they arrive; do not quote the
> 343x split as the bug's final magnitude.


Each rung is one render pair and each eliminates a whole class:

1. **Intra-process, same view repeated** — `--shotlist` with N identical lines
   reuses ONE world load and resets `m_frameIdx` per shot. If two identical
   views inside one process differ, the cause is per-frame (a race), and every
   "uninitialised process state" theory is dead. Measured 2026-11-12 on the
   mesh alpaca: 3,838–4,957 px between the pairs of four identical views.
2. **CPU inputs, logged per frame** — hash the uploaded table every frame, not
   just frame 0 (`VF_TRACE_MESH_HASH` hashes the whole `MicroWorldRow` array).
   `158910ef5c2166c9` on all five frames means the rig, the pose and the freeze
   hook are exonerated and the divergence is GPU-side.
3. **Post effects off** — `VF_RENDER_FLAGS=415` (SSAO + SSR). If the diff
   survives, the pass output itself varies and the post chain only amplifies.
4. **The feature off** — `VF_FUR=0`. Nearly-equal diffs with the coat on and
   off (4,533 vs 4,518 px) say the coat is not the author.
5. **The reference backend** — `--mode svo`. Differing on both backends rules
   out anything specific to the splat raster.

## The freeze string's slot order is a trap

`VF_TEST_BEHAVIOUR_FREEZE` parses
`name:dx,dy,dz:headingDeg:gait:graze:clipTime` — **seven** positional fields, and
`sscanf` stops at whatever the string supplies. Two sessions read the same
documented example as `…:gait:graze` and passed a graze value into the
**clipTime** slot. The result is not an error: the app accepts it, the animal is
frozen, and the pose is the one you did not ask for, so every number downstream
is plausible and wrong. Same class as keying mesh half-edges by direction. Check
the parser (`micro_objects.cpp`, the ` FreezePose` block) before trusting a
string you composed from memory, and state the whole spec in a note when you post
a pose-dependent number.

## Verify the instrument before believing the number

Three void pairs in one session, none of them the code's fault:

- A killed render poisons the **driver**, not just the run: a later
  `vkCreateDevice` can return `VK_ERROR_DEVICE_LOST` (-4) minutes after the
  kill, failing an unrelated render (measured 2026-11-12: a peer's
  SIGTERMed submit died at 15:19:01, and five minutes later an innocent
  `micro_check` rotation render failed to create a device on the same
  `VkResult -4`). If a pair dies with a create failure, look **backwards for a
  kill**, not sideways at a peer's build — and a fresh process launch, not a
  retry in the same process, is what clears it.
- A peer's **shader** build landing between render 0 and render 1 of a pair
  changes the second frame only. `ninja` recompiles `build/shaders/*.spv` for
  anyone who builds, so md5 the binary **and** the `.spv` files around the pair.
- A SIGKILLed gate leaves `assets/world.json` with the micro layers
  `enabled: false` and the alpaca's `behaviour` popped (see
  [[concepts/authored-assets-are-build-outputs]] for why the manifests are
  build outputs). A render with no animal is pixel-identical by construction.
  **Grep the log for `micro: 'micro/alpaca.vxw'` in every arm, every time.**
- A peer's GPU contention (see [[concepts/measurement-discipline]]).

## Where the divergence is born: the raster mesh pass

The **measured** fact is the fork, not the mechanism. `VF_MESH=0` draws the same
alpaca, same camera, same pinned pose, from its traced FIELD — a compute pass
with no raster stage — and that pair reads **0 px / max 0**, while the raster
pair on the same binary in the same session reads **4,278 px / max 141**. One
binary, one session, state hashes identical before and after. That arm is worth
more than the six that preceded it because it forks the *pipeline* rather than
the environment.

What follows the fork is a hypothesis about the raster pass, and its first
form is already half-falsified. The suspect is the write model: with culling OFF
a surface double-covers most pixels, both walls execute, both store, and a late
depth test does not un-store the loser, so the surviving value is whichever store
landed last. It is not a memory-ordering hazard, so no barrier, snapshot or
queue drain can touch it — the race is inside ONE draw call, which is exactly
what the six environment arms said.

### The candidate mechanism, and the fix that did NOT land

The obvious suspect is the write model: with culling OFF a closed surface
double-covers most pixels, both walls execute, both store, and a late depth test
does not un-store the loser, so the surviving value is whichever store landed
last. The textbook fix is to force the depth test ahead of the shader:

```glsl
layout(early_fragment_tests) in;   // after #version/#extension — before
                                   // #version it fails with "Missing entry point"
```

**That fix is declared, compiled and inert.** `spirv-dis` shows
`OpExecutionMode %main EarlyFragmentTests`, `--selftest` passes, the pipeline
accepts the mode — and the confirmation pair came back **4,653 px against its
4,278 px baseline** (fur-ON 3,980 vs 4,533). Vulkan has no `earlyFragmentTests`
member in `VkPhysicalDeviceFeatures`, so there is nothing to query: the shader
line is the entire host-side change, whether the driver honours it is
driver-observed, and on this driver it does not. The line stays as documentation
of intent, not as a cure.

Three CPU audits of `assets/models/alpaca_mesh.stl` (18,046 triangles) that
matter for what to try next — with one correction, because the first version of
this section published a false "open surface" claim:

- **no depth ties to break**: 0 duplicate triangles (independent recount after
  Mesher's census), 0 coplanar pairs.
- **per-triangle winding agrees with the stored normal in 18,046/18,046** — but
  that test only checks a triangle against its OWN normal field, which an
  exporter computes from that triangle's winding, so it can never detect an
  orientation-inconsistent surface. The adjacency test does: **7,008 of 26,897
  shared edges (26.1 %) are traversed in the SAME direction by both neighbours**,
  i.e. those patches face the opposite way from the surface around them. The
  pipeline's culling-OFF decision is therefore correct, and for the real reason —
  not "the exporter's winding might disagree" hand-waving, but a measured quarter
  of the surface actually inverted.
- **the surface IS closed.** The audit that said otherwise keyed half-edges by
  *direction*, which counts the two traversals of one welded edge as two edges,
  so a closed welded mesh reports nearly everything unpaired — the classic false
  positive for "not manifold". Keyed undirected: multiplicity `{2: 26897, 4: 86}`,
  and 26897·2 + 86·4 = 54,138 = every half-edge exactly, so 0 boundary loops. The
  86 edges at multiplicity 4 are non-manifold edges (four incident faces), not
  duplicated triangles — there are none.

### The cull fork: half the divergence went away, and the half that stayed is informative

`VF_MESH_CULL=1` (env-gated `VK_CULL_MODE_BACK_BIT` on the mesh pipeline, a
diagnostic only — the image is wrong by design because 26.1 % of the surface
faces inward, so culling deletes the outer wall there) gives
**one wall per pixel instead of two** wherever the double cover was a front/back
pair. Same binary (7077a2fb), same camera, same session, both pairs verified with
the animal present, state hashes identical before and after:

| arm | pair diff |
| --- | --- |
| unculled (same-session baseline) | 4,955 px, max 143 |
| `VF_MESH_CULL=1` | **2,345 px, max 141** — 47 % remains |

Turning off about half the double-covering removed about half the
nondeterminism. That is a causal, directional result for the overlap mechanism,
and it is the first arm in this whole investigation that MOVED the number.

The residual is not where a naive guess would put it. Only 6 % of the culled diff
pixels sit within 2 px of a sharp image edge, against a 5 % base rate for the
region — statistically indistinguishable from random. But that was the wrong
probe: a leg held in front of a belly is an **area** overlap, not an edge, so its
double-covered pixels are spread through the limb's interior. So the residual is
consistent with the front-face-against-front-face overlaps that culling
structurally cannot remove, and it rules out the tie/silhouette variants, which
would have been edge-pinned.

### The magnitude is unexplained, and two predictions died on it

The mechanism has real qualitative support — `VF_MESH=0` reads 0 px, culling halves
the number — but **every quantitative model of its size has now been falsified by
instruments built to confirm it.** A CPU rasteriser of the rest mesh at both
cameras (exact plane/ray intersection, not affine-interpolated depth):

| census | flk cam | furcheck cam | ratio | pair-diff ratio |
| --- | --- | --- | --- | --- |
| body area | 28,657 px | 13,072 px | 2.19 | **343** |
| double-covered area | 31,449 px | 23,935 px | 1.31 | 343 |
| near-tie < 1 mm | 10 px | 1 px | 10.0 | 343 |
| near-tie < 5 mm | 24 px | 10 px | 2.4 | 343 |
| near-tie < 50 mm | 1,423 px | 303 px | 4.7 | 343 |

Near-tie pixels — candidates whose two surfaces are close enough that `LESS` is
decided by interpolation noise rather than geometry — were the best surviving
story: the shape is right (the ratio grows as the gap tightens, 2.2 at 10 mm to 10
at 1 mm, and an edge-on view does converge the two walls where a head-on view does
not) but the size is two orders of magnitude short. Nothing in the projected
geometry explains a factor of 343.

And the pose sweep says the same from the other side: seven poses at the flk camera
span only 3,891–5,147 px, a ±15 % band whose extremes track `clipTime` rather than
gait, while one camera change moves it 5,147 → 15 px. **The view is essentially
everything, the pose is nearly nothing, and no geometric count I can compute tracks
it.**

**The third prediction died the same way, and it was the last cheap one.** If the
surviving store is decided by *which wall differs more from the other*, the
eligible pixels should be the ones where the two candidate surfaces disagree
most strongly. Measured on the flicker-camera pair: the pixels that changed in
**both** of two independent pairs (the race's stable core) had a median luma
difference of **27.0**, while the pixels that changed in only one arm had
**24.0** — a 12 % gap on a scale where the near/far walls differ by up to ~140.
So the selection is not driven by the size of the disagreement either, and the
one variable left standing is **eligibility**: which pixels can be written twice
at all.

So hold the mechanism as *qualitatively supported, quantitatively unexplained*. The
structural fix is still the right next move, but on the construction argument — a
failing fragment's framebuffer writes are suppressed, its side effects are not — and
**not** on a fitted magnitude. Do not upgrade the earlier 4,278/4,955/5,147 figures
into a predicted law, and if the attachment change lands at 3 k px instead of 0,
that is consistent with everything here rather than a surprise.

Independent of the magnitude question, the orientation census is worth having in
`vf_micro_mesh_check`: report **same-direction shared edges** as the headline
number for an incoming actor mesh (26.1 % here, 7,008 of 26,897). It is the
measured reason culling stays off, it is what an authoring-side weld-and-orient
pass would remove, and it is the number that would have stopped three separate
wrong theories in this session if it had existed first.

### The set test: it is a race, established without a single new render

A count that repeats and a **set** that moves is the signature of an ordering race;
a deterministic cause moves the same pixels every time. Three fully independent
pairs at the flk camera — same pose spec, same `VF_FUR=0`, differing only by
binary — already existed on disk: 4,653 / 4,955 / 5,147 px. Jaccard over the
changed-pixel masks inside the bbox: **0.237, 0.231, 0.229**.

Calibrate before interpreting: two random masks of ~4.8 k px over a ~31 k px body
score ≈0.084, and a deterministic cause would score ≈1.0. Observed 0.23 is 2.7×
chance and nowhere near 1.0 — so some pixels are *more eligible* than others, but
which ones actually move is not fixed. Secondary, and weaker because a pose change
legitimately moves pixels: gait0-vs-control 0.151, gait0-vs-gait180 0.225, i.e. the
set stays about equally scattered across half a stride.

That matters more than any of the census work, because it is the premise the
structural fix rests on: a framebuffer write is suppressed when a fragment fails
the depth test **only helps if the failure is real**, and a nondeterministic order
is what makes it real. It also sharpens what is still missing: the camera
dependence is now a question about **how many pixels are eligible** (~5 k at one
viewpoint, ~15 at another) rather than about which move, and no eligibility model
survives yet — area, near-tie counts and pose have all been measured and falsified.

## Why every environment arm died, and what each one still costs

Six arms, same camera, same pose, `VF_FUR=0`, animal verified present:

| arm | pair diff |
| --- | --- |
| splat world (control) | 4,792 px |
| `--mode svo` world | 4,892 px |
| `VF_NO_GPU_CULL=1` | 4,641 px |
| `VF_NO_OCCL=1` | 4,966 px |
| queue drained per submit | 4,518 px |
| per-slot depth attachment | 4,843 px |

All nonzero, all ~4.5–5 k, all inside the same spread — so the world backend,
the GPU cull pre-pass, the Hi-Z pyramid, cross-frame overlap and the depth
attachment's clear ordering are each **excluded by measurement**, not by
argument.

Two things found on the way are still true and still in the tree.

**1. Barriers around the micro pass must name `FRAGMENT`.** The mesh writes
`hdr`/`gpos`/`gnorm` from the fragment stage while the barriers named only
`COMPUTE`, a real write-after-write hazard against the world's colour-attachment
stores on exactly the pixels the mesh covers. Fixed in all three frame paths and
in `FurPass::record`; it did not move this number (4,322 px before, 4,533 px
after) and it was correct independently of that.

**2. A resource a frame destructively writes must be indexed by the in-flight
slot.** `kMaxFramesInFlight` is 3 (`src/app/app.hpp`); the frame loop waits on a
slot's fence when it CLAIMS the slot, which proves the submission **three frames
back** complete and nothing else, and a barrier inside a command buffer cannot
reach across submissions at all — the same-layout `transitionImage` that used to
sit in front of the mesh depth clear claimed exactly that and did nothing. So
`MicroSvoPass::record` now takes the frame slot and the depth attachment is
`m_meshDepth[kMeshSlots]`. This closes a latent defect; it was **not** this
flicker's cause (4,843 px after the change).



## See also

- [[concepts/micro-voxel-svo-overlay]] — what the micro pass writes and where.
- [[concepts/fur-raster-parity]] — the fur's barrier chain and its rect bug.
- [[concepts/measurement-discipline]] — contention invalidates a verdict in
  either direction, so a contended PASS is not evidence either.
