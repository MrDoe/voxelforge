---
title: Oriented brush rasterizers — project the reach on the axis, never on world Y
tags: [live-edit, rasterizer, floating-point, brush, glm, correctness]
sourceRefs: [src/voxel/editable_world.cpp, src/voxel/editable_world.hpp, src/app/main.cpp, shaders/splat.frag, tests/test_editable.cpp]
lastReviewed: 2026-10-03
---

# Oriented brush rasterizers

`EditableWorld` stamps three axis-oriented volumes: `makeOrientedCylinder`
(Carve) and `makeDome` (Add) take an `axisDir` (the picked surface normal),
`makeSphere` (Delete/Paint) is axis-free. The oriented pair shares three rules
that are easy to get wrong and invisible when you only test on flat ground.

## 1. The loop bounds must be projected on `axisDir`

The obvious way to bound the cell loop is a world-space AABB with the *reach*
on Y, because the shape is rasterized in a local frame whose axis is +Y:

```cpp
const int y0 = toCell(base.y),            y1 = toCell(base.y + heightM); // WRONG
```

That is only correct for an up-facing surface. For a wall (normal ±X or ±Z) it
clips the footprint — which lies in the plane *across* the axis — to
`y >= base.y`, so the brush covers nothing below the picked cell. Measured on
the cabin's front wall with the default 2 m diameter, at depths 0.5 / 1.0 /
1.5 m, cells emitted / of those below the pick:

| depth | old (world-Y bound, half-ellipsoid) | new (projected, extruded + fillet) |
|---|---|---|
| 0.5 m | 479 / **0** | 1726 / 805 |
| 1.0 m | 1206 / **0** | 3027 / 1408 |
| 1.5 m | 1748 / **0** | 4612 / 2148 |

Project the reach instead:

```cpp
const glm::vec3 half(radiusM + reach * std::abs(axisDir.x),
                     radiusM + reach * std::abs(axisDir.y),
                     radiusM + reach * std::abs(axisDir.z));
```

Add **one cell of slack** per side: the box edge lands exactly *on* the
footprint's boundary cell and `toCell()` floors, so a sum one ulp low drops
that outer ring. The shape test rejects the extra cells; the loop only has to
visit them.

Note what a *render* assertion can and cannot see here: head-on and oblique
before/after frames differ by only 2.50 % (old) vs 2.68 % (new) pixels, because
a bulge and a slab of the same brush look nearly identical from outside. Gate
this in `tests/test_editable.cpp` on the cell set, not in
`tests/live_edit_check.py` on pixels.

## 2. Never branch a shape test on a plane where the branches disagree

`glm::rotation(up, axisDir)` is degenerate for an axis exactly opposite to up
(GLM picks an arbitrary perpendicular, the basis tilts by ~1e-7), so any exact
`d > 0` test on a shape rasterized in that frame is a floating-point coin
flip. Carve hit this first and is fixed with a one-voxel `kCarveTopMargin` plus
a `VOXEL * 1e-3` keep-epsilon on the boundary.

Add hit the same class of bug twice, and the general fix is a *shape* rule,
not an epsilon: make the profile branches **meet** at the switch plane, so a
1-ulp sign flip there cannot change the result. Concretely the Add volume is an
extruded footprint disk closed by a fillet of radius `c = min(radius, depth)/2`:

| axial offset `t` | cross-section radius |
|---|---|
| `t <= depth - c` (straight side) | `radius` |
| `depth - c < t <= depth` (fillet) | `radius - c + sqrt(c² - (t - (depth - c))²)` |

Both branches give `radius` at `t = depth - c`, so the switch is safe. The two
rejected alternatives were (a) a plain half-ellipsoid — a bulge that tapers to
nothing at the rim, so a "thicker wall" reads as a bump, and (b) a disk
extruded with a hemispherical cap of `min(radius, depth)` — which is
*discontinuous* for `depth < radius` (radius `R` at the base, `depth` just
above it) and put the fp-coin-flip right on the base plane, where it dropped
half the footprint ring.

## 3. Keep the CPU volume and the GPU preview as one shape

The hover tint (`BrushUBO` bind 13) must be the *exact* set the CPU rasterizer
emits, or the preview lies. The encoding is a super-set of the other brushes:
`bVolume.w` = brush radius, `bAxis.xyz` = unit axis, `bAxis.w` < 0 = Add with
`|bAxis.w|` = growth depth, 0 = ball, > 0 = cylinder half length. A 0.06 m
skin is folded into the radius and the depth by the caller (surfels sit
0.05 m out along their normal), so the preview is slightly larger than the
cell set on purpose. `inBrushVolume` in `shaders/splat.frag` repeats the
branch structure above; change one, change both, and extend
`tests/test_editable.cpp` with the profile landmarks (centre reach, footprint
edge reach, one cell past the fillet, one voxel behind the surface).

## Shape choices worth keeping

- **Extruded footprint + fillet, not a dome**: the brush parameters mean what
  they say — diameter = footprint, depth = reach — and a thickened wall stays a
  wall instead of becoming a bulge.
- **Nothing behind the surface** (one voxel of base layer excepted, which seals
  the growth against the surface exactly as the carve scoop's base layer does).
  Clicking the near face of a thin wall must not erode the far side.
- **Rotation-symmetric profiles are free**: GLM's arbitrary perpendicular basis
  never shows, so the axis may be any unit vector.

## 4. Below one voxel, leave the rasterizer entirely

The volume shapes carry two deliberate tolerances: the cell-face epsilon
(`-eps` = one voxel behind the surface) and the fillet/cap, both of which reach
*into the neighbouring cell along the axis*. At a 1-voxel brush that is the
wrong answer — measured on this same shape: `makeDome(r=0.05, h=0.1)` emits
**2** cells and `makeOrientedCylinder(r=0.05, carve)` emits **4** (the carve
additionally reaches `kCarveTopMargin`). So the smallest brush widths must
short-circuit to an explicit single-cell edit rather than be rasterized: see
`EditableWorld::makeSingleVoxel` and the `perVoxel` branch in
`App::applyEditLive`. Any new brush shape inherits this: check what the
tolerances do at minimum size before assuming the shape still means "one voxel".

## 5. Radial falloff is a NAMED CURVE (2026-10-03)

Add, Carve, Delete and Paint take a `FalloffCurve`: `Constant, Sphere, Root,
Smooth, Linear, Sharp`. All satisfy `f(0)=1`, `f(1)=0`, monotone. The
half-radius value is how you choose one:

| Curve | `f(q)` | at `q=0.5` |
|---|---|---|
| Constant | `1` | 1.00 — flat; the legacy footprint, and flat-bottomed digs |
| Sphere | `√(1−q²)` | 0.87 — round dome |
| Root | `√(1−q)` | 0.71 — broad shoulder |
| Smooth | `1−3q²+2q³` | 0.50 — default |
| Linear | `1−q` | 0.50 — cone |
| Sharp | `(1−q)²` | 0.25 — crease |

**Why named curves and not one scalar.** The previous single `0..1` slider had
a cliff and no usable middle: `falloff <= 0` returned **exactly 1.0**, while
`falloff = 0.001` was already `((1+cos πq)/2)^1.001` — **0.5 at half radius**.
A 0.001 nudge jumped from "no taper at all" to "halved", and the rest of the
travel only steepened toward a spike (`k=3` → **0.125** at half radius). That
is why the control read as inert and then confusing; the shape was fine, the
*parameterisation* was not. `Constant` is now a real curve rather than a
special case, which is what removes the discontinuity.

**Polarity rule for any exponent form:** for a base in `[0,1]`, `u^k` *shrinks*
as `k` grows, so `k` must **increase** with falloff. Mixing it backwards
inverts the control — measured: `falloff 0.25` gave `0.00531` at the rim where
`0.05` gave `0.000694`, i.e. more "falloff" meant *more* influence.

**No exponent form is needed now**, but the cosine base's quadratic rim
(`f ~ (π²/8)(1−q)²`) was chosen for a second reason that still holds: it gives a
wide band of rim columns reliably below half a voxel. A profile that drops
linearly at the rim puts the "does this column emit" decision on a
floating-point knife edge and the footprint boundary flickers between stamps.

- **Add**: `hf = heightM · f(q)`, fillet `cf = 0.5·min(radiusM, hf)`. The two
  branches still agree at this column's own `lipY`, which keeps the taper from
  tearing the surface into rings. A column whose tapered height cannot hold half
  a voxel emits nothing.
- **Carve**: **only the far end tapers.** `kCarveTopMargin` is what opens the
  ground the scoop starts at, and fading it puts the one-cell roof back. A
  one-cell floor (`max(lengthM·f(q), VOXEL)`) keeps the rim crisp.
- **Delete/Paint**: `makeSphere` scales the *radius* per cell, so a Delete
  becomes a graded crater. `Constant` is the original hard ball.

`Constant` is the default for every rasterizer, so every existing call site
(vf_mcp, tests) is bit-for-bit unchanged; the App defaults its control to
`Smooth`.

### The mark is smaller than Width, and the panel says so

The visible mark **shrinks below the nominal Width** whenever the curve tapers,
because the curve reaches zero at the rim. That is inherent, not a bug — but it
is *confusing* unless stated, so the Edit panel prints the reach at which a
column still clears half a voxel (`reach 0.42 m (0.75 m footprint)`) next to
the Width slider, and plots `f(q)` with guides at `q=0.5` and `f=0.5`.

Per-voxel mode bypasses the rasterizers, so a 1-voxel brush still edits exactly
one cell (`check_per_voxel`).

## 6. The hover preview repeats the curve (bMeta.y)

The tint volume is a *second implementation* of the same shape, so it was taught
the taper: the **curve index** rides `bMeta.y` (previously unused) of
`BrushUBO`, set through `SplatPass::setBrush` / `SvoPass::setBrush`, and read by
`sharedInBrushVolume` via `brushFalloffCurve`. The dome branch scales its
per-column height and fillet; the cylinder branch maps `[-w, +w]` onto
`[−w, w·f]` so the near end stays untapered; the ball branch grades the radius.
`test-preview` is the gate — any change to `inBrushVolume` must run it.
