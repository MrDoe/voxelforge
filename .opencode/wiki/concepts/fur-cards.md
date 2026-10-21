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

## The three decisions, and the measurements behind them

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

## What the numbers were

| quantity | value |
|---|---|
| the alpaca's cards | **81 848** (4 per surface cell) |
| the card buffer | 7.9 MB on the GPU |
| the generation | **177 ms** at load |
| the frame cost | 0–1 ms at 900×600 (within the run-to-run noise) |
| the near view's changed pixels | 16 % of the frame, mean \|diff\| 53 |
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

## Gates

- `tests/fur_check.py` (the `fur` group) measures the **noise floor first**
  (two identical renders; 0 pixels above 6/255), then the signal (the
  alpaca's pixels must change), the **confinement** (the change must be a
  compact blob, and the house view must be untouched), the
  **backend-independence** (the SVO reference must show it too — the fur is
  its own pass, not a splat-backend feature) and the **escape hatch**
  (`VF_FUR=0` must load no fur object and render the same frame).
- `tests/test_micro.cpp` gains four cases: only a fur material grows a coat,
  the roots sit **on** the surface while the strands point **out** of it, two
  builds of the same field yield the same cards (determinism), and every
  card is filed in a bucket cell that holds it.

Cross-refs: [[concepts/micro-voxel-svo-overlay]] (the objects the fur grows
on), [[concepts/splat-base-seal]] (why the world's splats cannot do this),
[[concepts/detail-pipeline]] (the micro-detail layer that was removed in
2026-10-08, and the difference between it and this), [[entities/svo-render]]
(the backends), [[concepts/measurement-discipline]].
