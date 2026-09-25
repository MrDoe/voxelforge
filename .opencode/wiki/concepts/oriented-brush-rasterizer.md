---
title: Oriented brush rasterizers — project the reach on the axis, never on world Y
tags: [live-edit, rasterizer, floating-point, brush, glm, correctness]
sourceRefs: [src/voxel/editable_world.cpp, src/voxel/editable_world.hpp, src/app/main.cpp, shaders/splat.frag, tests/test_editable.cpp]
lastReviewed: 2026-09-25
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
