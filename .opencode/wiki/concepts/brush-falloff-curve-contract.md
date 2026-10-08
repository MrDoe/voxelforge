---
title: Brush falloff curve — reconstruction contract
tags: [brush, live-edit, falloff, spec, reconstruction]
sourceRefs: [src/voxel/editable_world.hpp, tests/test_editable.cpp, src/app/edit/live_edit.cpp, src/app/ui/panel_edit.cpp, src/app/frame/run_brush_preview.cpp]
lastReviewed: 2026-10-08
---

# Brush falloff curve — reconstruction contract

The three `curve`-taking bodies in `src/voxel/editable_world.cpp` were lost to a
branch checkout in a dirty tree on 2026-10-08 (see
[[concepts/uncommitted-edit-is-not-yours]]). **Everything needed to rebuild them
is still in the tree**, and this page is that material collected in one place so
the next session inherits a spec instead of re-deriving one.

## What is missing, and what is not

**Intact** — all inline in the surviving `.hpp`, never in the `.cpp`:

| symbol | form |
|---|---|
| `enum class FalloffCurve` | `Constant, Sphere, Root, Smooth, Linear, Sharp, Count` |
| `falloffCurveName(FalloffCurve)` | `static`, body in header |
| `falloffCurveAt(FalloffCurve, q)` | `static`, body in header — **all six curves** |
| `radialFalloff(q, float)` | `static` legacy 0..1-slider bridge |

`git show HEAD:src/voxel/editable_world.cpp | grep -c FalloffCurve` → **0**.
So no curve math was lost.

**Missing** — the `.cpp` bodies behind three overloads declared in the `.hpp`:

```cpp
makeOrientedCylinder(anchor, axisDir, radiusM, lengthM, mat, carve,
                     FalloffCurve curve = FalloffCurve::Constant)
makeDome(anchor, axisDir, radiusM, heightM, mat,
         FalloffCurve curve = FalloffCurve::Constant)
makeSphere(anchor, radiusM, mat,
           FalloffCurve curve = FalloffCurve::Constant)
```

`makeSingleVoxel` does **not** take a curve and is unaffected.

## The curve table

`q` = perpendicular distance / radius, so centre `q=0` → full influence, rim
`q=1` → zero. Every curve satisfies **f(0)=1, f(1)=0**, is non-increasing, and is
continuous at `q→1` — so the "does this cell emit" test is never on a
floating-point knife edge at the boundary.

| curve | f(q) | f(0.5) | reads as |
|---|---|---|---|
| `Constant` | `1` | 1.00 | flat — the legacy footprint |
| `Sphere` | `sqrt(1-q²)` | 0.87 | round dome |
| `Root` | `sqrt(1-q)` | 0.71 | broad shoulder |
| `Smooth` | `1-3q²+2q³` | 0.50 | default; sculpt-standard S |
| `Linear` | `1-q` | 0.50 | straight cone |
| `Sharp` | `(1-q)²` | 0.25 | crease / tight |

**`Constant` is the default on all three**, and it reproduces the pre-curve shape.
That is the safety property: a correct re-implementation leaves existing behaviour
unchanged, so divergence is caught by the tests rather than shipped.

## The three applications differ, and conflating them is the trap

Each function applies `f` to a **different quantity**. This is stated in the
`.hpp` comments and is the part most likely to be got wrong.

| function | `f` scales | consequence |
|---|---|---|
| `makeOrientedCylinder` | the **far end** of the volume, per rim column | scoop deepest under the cursor, feathering at its edge instead of leaving a vertical crater wall |
| `makeDome` | the **height per column**, *and the fillet shrinks with it* | growth becomes a mound — full `heightM` at centre, nothing at rim |
| `makeSphere` | the **radius per cell**: include when `dist ≤ radiusM * f(q)` | Delete goes from a hard ball to a graded crater |

### The carve exception — do not taper the near end

`makeOrientedCylinder` must **not** taper the near end. `kCarveTopMargin`
(`2 * VOXEL`) is what opens the ground the scoop starts at; fading it puts the
one-cell roof back over the dig — and a covered void gains no water.

### The rim rule needs a reach FLOOR, not just an exception

The prose rule is "a rim column still cuts **one** cell, so the footprint
boundary stays crisp." That is easy to re-derive wrong, because
**`f(1) = 0` makes it fail silently.**

With the taper applied naively, a rim column's reach collapses to exactly the SDF
test's own threshold, and the boundary cells drop out of the emitted set. Measured
on reconstruction (2026-10-08): **2814 of 2821 cells survived — 7 boundary cells
silently missing**, which reads as a slightly-off carve rather than as a bug.

> **Floor the taper's reach at `kCarveTopMargin + VOXEL`, plus the SDF test's own
> epsilon.** That is what "a rim column still cuts one cell" means in code: the
> taper must never reduce the reach below one voxel of the un-tapered extent.

This is the general trap of the feature: **`f(1) = 0` is continuous, which is good
for shading and bad for a hard inclusion test.** Any predicate of the form
`emit if <threshold>` combined with a taper that reaches exactly that threshold
loses the boundary — and the loss shows up as a slightly-off number rather than as
an error.

### The dome's mirror image: grade the test, not just the quantity

Verified 2026-10-08 (`test_editable` green, 13/13 cases, 3013/3013 assertions):
the dome needed the same class of fix from the opposite side. Tapering the
per-column height is not enough, because the straight-disk branch tests the
**ungraded** `perp2 <= r2` — so the rim column kept its base cell even at full
taper (measured rim reach 1, now pinned as nothing-at-rim).

> **Gate the kill on the grade:** a graded column shorter than half a voxel
> (`colHeight < 0.5 * VOXEL`) emits nothing — but only when a taper is active.
> The `Constant` path must keep its exact legacy footprint, so the kill is
> conditional on grading, never unconditional.

The pair is now symmetric: the carve floors the *reach* (so the rim still cuts
one cell), the dome kills the *column* (so the rim emits zero cells). Both are
the same rule — **the inclusion test must see the tapered quantity, never the
un-tapered one** — applied at the two ends where each function tests membership.

### The sphere needs no fix, and the reason is structural

Same verification run: the sphere's test is `dist <= R * f(q)`, which grades
**both sides of the comparison together**. There is no ungraded branch for the
boundary to leak through, so the class cannot bite. This is the cheapest kind
of correctness argument — not "tested and holds" but "not expressible" — and
it is worth preferring wherever the predicate can be written that way: a
comparison that grades both sides needs no floor and no kill.

### The Add decision was re-chosen on purpose

`makeDome`'s flat top was **deliberately reversed**: a full-height rim is right
for "thicken this wall" and wrong for "raise this spot", so the growth becomes
proportional. The `.hpp` says explicitly **do not "restore the flat top"**.

### Parity obligation

The hover preview tints the same set the stamp emits, via `BrushUBO` with
`w < 0` on the axis for Add. **The shader repeats the extruded-disk + fillet test,
and the curve.** A correct CPU implementation with a shader that does not apply
the same curve produces a preview that disagrees with the stamp.

See [[concepts/oriented-brush-rasterizer]] for the loop-bounds rule (project the
reach on `axisDir`, never on world Y) and [[concepts/brush-preview-visibility]]
for the preview gate.

## Acceptance is already pinned — and now met

**Status 2026-10-08: IMPLEMENTED AND VERIFIED — boundaries closed.** `test_editable`
green, **15/15 cases, 3037/3037 assertions**, both boundary fixes above in the
tree plus the dome/sphere rim exact-count checks. The gap named below ("only
the carve could have caught the rim class") is shut: every tapered boundary is
now proven, not just the carve one.

`tests/test_editable.cpp` holds the numeric expectations — the half-radius column
above, the `f(0)`/`f(1)` boundaries, the `Constant == legacy identity` check, and
call-site cases exercising `Sphere`/`Sharp`/`Constant` through all three
functions. Run them as the definition of done; they fail loudly rather than
quietly approximating.

**On the assertion count 3053 → 3013:** same 13 cases, fewer emitted cells.
The count comes from per-record `CHECK` loops, so fixing the boundary (fewer
cells emitted) lowers the count by construction. A falling assertion total next
to a boundary fix is the suite getting *tighter*, not thinner — see
[[concepts/measurement-discipline]] on why a count is not evidence either way.

### Why only the carve caught the rim class — and what that leaves open

Fledge's distinction, filed 2026-10-08 because it decides what "green" proves:
the dome and sphere tests assert reach **monotonicity and proportional
shortening**, not an exact boundary cell count — so a lost boundary cell there
**does not fail**. The carve was the only one with an exact count (`found ==
expected` over the whole top-layer disk), which is why it caught the
2814-of-2821 class and the others could not have.

Consequence (closed 2026-10-08): the boundary-count checks now exist and pass —
**15/15 cases, 3037/3037 assertions**. Dome rim ring reads zero across all five
tapered curves, with the legacy path guarded non-zero (the kill stays gated on
grading, exactly as specified); sphere sits a full cell inside with `Constant`
bit-identical. The tapered dome/sphere boundaries are now **proven**, not just
the carve one — the gap this subsection named is shut. (Authorization for those
checks came from this page; implementation stayed in Fledge's `test_editable` +
clean-build gate throughout.)

Two compile-time guards from the same session are worth copying into any future
reconstruction: `static_assert` on the types a layout depends on, and buffer
sizes tied to a derivation rather than written as a literal. See
[[concepts/measurement-discipline]].

## Legacy scalar bridge

`radialFalloff(q, falloff)` maps the old 0..1 slider onto a curve
(`≤0` → flat; `<0.2` Root; `<0.45` Sphere; `<0.8` Smooth; else Sharp). It exists
because the old scalar **had a cliff**: `falloff <= 0` returned exactly `1.0`,
while `falloff = 0.001` was already 0.5 at half radius — so a 0.001 nudge jumped
from "no taper" to "halved", and the rest of the travel only steepened toward a
spike. That is what made the control read as inert and then confusing. New code
passes a `FalloffCurve` directly.
