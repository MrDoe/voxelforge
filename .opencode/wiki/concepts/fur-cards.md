---
title: Fur cards — a translucent hair layer over the micro objects
tags: [rendering, micro-voxel, fur, translucency, splats, alpaca]
sourceRefs: [src/voxel/fur_cards.hpp, src/voxel/fur_cards.cpp, src/render/fur_pass.hpp, src/render/fur_pass.cpp, shaders/fur_overlay.comp, src/app/world/micro_objects.cpp, src/app/frame/record_interactive.cpp, tests/fur_check.py, tests/test_micro.cpp, AGENTS.md, docs/rendering.md]
lastReviewed: 2026-10-21
---

# Fur cards

The alpaca is **not** rendered from the world's Gaussian surfels. It is a
**micro-voxel object** (`world.json` → `role: "micro"` → `MicroField` →
`MicroWorld` → `MicroSvoPass` → `shaders/micro_overlay.comp`): a sphere-traced
solid on its own 8 mm lattice, composited into the G-buffer where it is nearer
than what the main backend resolved. See
[[concepts/micro-voxel-svo-overlay]].

That matters for the question "can we use real splats for the fur?", because
the premise ("the alpaca is a chain of filled circular surfels") is false. Its
surface is a sphere-traced 8 mm SDF — exact, opaque and hard-edged. Right for
a ceramic vase, wrong for an animal, whose coat is fuzz and whose **silhouette**
is the thing that says so.

## The shape of the answer

The fur is a **separate, translucent layer of hair cards**:

- a **card** is a flat ribbon standing *out* of the surface — 3 cm long,
  2.5 mm wide, tilted 25–60° off the surface, groomed down it, with a
  **Gaussian across the width** (the splat backend's kernel shape, on a sliver
  instead of a disk) and a taper along the length. One card stands for a clump
  of strands, so the cost scales with clumps, not hairs.
- the cards are **generated on the CPU** at load from the field's *surface
  cells* (a solid cell with a non-solid neighbour) whose material grows fur —
  **21 `fleece`**. The vase and the urn (20 plaster) grow nothing.
- they are drawn by a **compute pass** (`shaders/fur_overlay.comp`) that runs
  after the micro overlay and composites into the **same HDR image**. The
  G-buffer's world position is the depth reference, so a card behind the body
  or behind the world is dropped with no depth attachment and no change to
  any opaque pass.

**Why not do it with the world's splats?** A surfel is a disk lying *in* the
tangent plane, and the opaque **base seal** (the nearest fragment written at
alpha 1) is exactly what turns a lone silhouette splat into a hard-edged
blob — the documented fix for that is a **coverage count** in the unused
`D24_S8` stencil, not kernel tuning (see [[concepts/splat-base-seal]]). The
fur sidesteps the whole question: it composites over an already-opaque body,
so transparency is correct by construction and the sky only shows where a
card reaches past the silhouette.

## The four decisions, and the measurements behind them

1. **The ribbon is billboarded about the strand** — its width axis is
   perpendicular to the strand *and* to the camera. A width axis taken from
   the surface puts the ribbon in the tangent plane, so at the **silhouette**
   — where the surface is edge-on to the camera, and where the fuzz is most
   wanted — the ribbon is edge-on too and vanishes. The coat would be a
   faint wash in the interior and the outline would stay razor sharp.
   Measured on the near view: the mean |diff| against the fur-off render went
   32 → 53 and the high-frequency noise 18.4 → 15.4 (the bare body is 12.8).
2. **A card is never thinner than ~1.5 px.** A sub-pixel card either misses
   the pixel grid or lands on it whole, and the coat turns to speckle. The
   card is widened to 1.5 px and its alpha scaled by the same factor, so the
   ink is conserved. Alone: the high-frequency noise 20.9 → 19.0.
3. **The cards must be dense** — four per surface cell. Two left gaps at
   close range; more buys very little (measured: the neighbour-difference
   metric 19.0 at two cards, 18.2 at four, 16.8 at six).
4. **The strands must be CURLY, and the amplitude must be a LARGE fraction
   of the length.** The centre line wanders across the ribbon's plane in a
   wave that is zero at the root (where the strand is anchored) and grows
   toward the tip. The ribbon's *plane* is unchanged, so the ray/plane
   intersection stays exact and the curl costs one sine. At an amplitude of
   0.16 the wave is a couple of pixels across on a fifteen-pixel strand and
   the coat still read as **spikes**; at 0.45 it reads as wool — the
   measured high-frequency noise fell 16.1 → **12.7**, below the bare body's
   13.5, because a curled strand spreads its ink instead of concentrating it
   in a needle.

## The bug the curl exposed — and it WAS the spikes

The strand's direction was spelled `groom*cos(spin) + t2*sin(spin)`, which
is **not a rotation** unless the groom happens to lie along `t1` — and the
groom is the projection of "down", so it generally does not. The length
varied with the spin, and at the spin where the two terms cancel the
direction was pure numerical noise: the card degenerated into a **spoke
pointing straight out of the surface**.

Measured, with the unit test added to catch it: the strand's angle to the
normal ranged **8.6°–89.8°** instead of the intended 12°–38°. Expressing
the groom in the surface's tangent frame and rotating *that* keeps the
length at one for every spin.

The lesson worth keeping: **the "spiky look" was not an aesthetic
parameter, it was a bug**, and it only became visible because a new
assertion (the strand must point out of the surface) was written while
changing something else. A visual complaint is a measurement request.

## The coat in the GUI

- The sidebar's **Rendering** section carries the coat's **shape**: density,
  strand length, clump width, curl amplitude and cycles, and how far a
  strand stands off the surface.
- The **Textures** section carries the **material** that wears it, because a
  coat is a property of the material and not of the object: a checkbox per
  material turns the coat on wherever a micro object uses it. The fleece
  (21) is the one that ships with a coat — and it was not even *visible* in
  that panel before, because the panel's loop stopped at material 20.
- The sliders **rebuild** the coat (the cards are a CPU product, a few
  hundred milliseconds) and apply on **release**, not per tick. A rebuild
  keeps the live Move/Rotate preview: that state lives on the fur objects
  and rides across the upload.

## What the numbers were

| quantity | value |
|---|---|
| the alpaca's cards | **122 772** (6 per surface cell) |
| the card buffer | 15.7 MB on the GPU (96 B per card) |
| the generation | ~200 ms at load |
| the frame cost | 0–1 ms at 900×600 (within the run-to-run noise) |
| the near view's changed pixels | 16 % of the frame, mean \|diff\| 56 |
| the coat's high-frequency noise | **12.70**, BELOW the bare body's 13.48 |
| the fur's effect at 8 m | the alpaca's pixels change; the house view does not |

## The traps

- **The alpaca's position was measured wrong twice.** A camera that "should"
  see it was actually pointing at a cabin wall, and the fur's effect at
  distance measured as *zero* — a broken instrument, not a broken feature
  (see [[concepts/measurement-discipline]]). The instrument has to be able
  to produce the signal before its absence means anything.
- **The texture is sampled PER CARD**, not once per pixel: the fleece's
  clumps are centimetres across, and one flat colour for the whole pixel
  reads as washed-out plastic. The **sky ambient is evaluated once per
  pixel** (the sun term is the cards' average) — a sky evaluation per card
  hit would cost a dozen per pixel through the coat.
- **The accumulation is the many-layer approximation** `1 - exp(-Σα)`, which
  is exact for the small alphas a coat is made of, and the coverage
  composites the fur *over* the body.
- **The cards live in the field's LOCAL frame**, so the layer's placement and
  the live rotation preview are applied by the shader exactly as the micro
  overlay applies them to the field. The fur and the body cannot drift apart.
  The preview is mirrored through an index map (`m_furIndexOfMicro`), because
  a micro object *without* fur has no entry in the fur pass's table.
- **The meadow got the same lesson** (`common_base.glsl`): a grass blade
  whose centre line is straight and whose width tapers to a point reads as a
  spike, so the blade now arches (quadratic, the bend growing toward the
  tip), keeps much more of its width, and fades over the last fifth instead
  of the last tenth. Measured on the house view: 1.74 % of the pixels move,
  the rest of the frame untouched.

## Gates

- `tests/fur_check.py` (the `fur` group) measures the **noise floor first**
  (two identical renders; 0 pixels above 6/255), then the signal (the
  alpaca's pixels must change), the **confinement** (the change must be a
  compact blob, and the house view must be untouched), the **shape** (the
  coat must be at least as *smooth* as the body under it — the numeric form
  of "curly, not spiky": a straight coat concentrates its ink in narrow
  streaks, and a narrow streak IS high-frequency energy, so it reads as
  crunchier than the body it covers; a curled one spreads the same ink and
  falls *below* the body's. With the curl removed the check fails), the
  **backend-independence** (the SVO reference must show it too — the fur is
  its own pass, not a splat-backend feature) and the **escape hatch**
  (`VF_FUR=0` must load no fur object and render the same frame).
- `tests/test_micro.cpp` gains five cases: only a fur material grows a coat,
  the roots sit **on** the surface while the strands point **out** of it
  (and their angle to the normal stays inside the requested range — the
  check the degenerate direction failed), two builds of the same field
  yield the same cards (determinism), and every card is filed in a bucket
  cell that holds it.

Cross-refs: [[concepts/micro-voxel-svo-overlay]] (the objects the fur grows
on), [[concepts/splat-base-seal]] (why the world's splats cannot do this),
[[concepts/detail-pipeline]] (the micro-detail layer that was removed in
2026-10-08, and the difference between it and this), [[entities/svo-render]]
(the backends), [[concepts/measurement-discipline]].
