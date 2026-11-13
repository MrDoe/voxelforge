---
name: voxel-animal-animation
description: Animate a walking animal in voxelforge's micro-voxel system using a derived skeleton/rig. Covers rig derivation (bones from voxels), gait (diagonal leg pairs), ground contact (no sliding/sinking), body bob/sway, neck chain, graze, and the ground clamp. Use when adding or fixing animal movement, walking, grazing, or skeleton animation in voxelforge.
---

# Animating walking animals in voxelforge

This skill covers the micro-voxel animal animation system: a **derived skeleton**
(`MicroRig`) drives a **behaviour transform** (`MicroBehaviour`) that is applied
in the shader, not by re-baking. The animal keeps its exact geometry while it
moves. The key invariant: **feet never slide or sink** — every step is planted
on the terrain.

## Architecture

```
MicroField (fine lattice) → MicroRig (derived bones) → MicroBehaviour
  (state machine + gait) → MicroSvoPass (behaviour transform + bone rotations)
  → micro_overlay.comp (sphere trace in the REST frame)
```

- `src/voxel/micro_rig.cpp` — derives bones from the object's own voxels.
- `src/voxel/micro_behaviour.cpp` / `.hpp` — state machine (walk/graze/stop) + gait.
- `src/app/world/micro_objects.cpp` — ground clamp, behaviour update, bone upload.
- `shaders/micro_overlay.comp` — applies behaviour + bone transforms at trace time.

## The rig is DERIVED, not authored

Nothing in the code knows what an animal is. `MicroRig::build` finds the
skeleton from the object's voxels. Three rules that cost a session each:

1. **Spine = the longer HORIZONTAL axis**, not the longest axis. An animal is
   usually taller than it is long (alpaca: 1.3 m tall, 1.1 m long), so the
   longest axis is vertical and the derivation finds NO legs. Measure both
   horizontal extents and pick the longer.
2. **Belly = a PERCENTILE of the spine band's cell heights**, not the minimum.
   A quadruped's front legs stand INSIDE the spine band, so the minimum is the
   hooves and nothing is "below the core" — the legs become part of the body.
   Use ~30th percentile.
3. **A leg reaches the ground.** The coarse coat hangs below the belly, and the
   drop test alone reads fleece locks as limbs. A cluster counts as a leg only if
   its bottom is within `max(2 cells, 6 % of height)` of the object's own bottom.

A bone is a **segment** plus a **role** (Spine, Leg, Head, Tail, Neck). The gait
is driven by the ROLES, so a cat animates the way a horse does. Bones form a
CHAIN: a bone's transform is its parent's composed with its own
(`R_w = R_p·R_l`, `t_w = R_p·t_l + t_p`). Legs, head and tail hang off the
spine.

The neck is a **chain of three links** (Neck, Neck, Head), not one bone. A
single `Head` bone turns the whole neck about its base — one hard crease, a
rigid pole, and a wedge opens on the outside of the bend. Spreading the bend
over three links curves the neck. `animateBones` divides `headTotal` by
`neckLinks`.

## Gait: diagonal pairs, not a trot

```cpp
// legs sorted along the spine: front legs first
const bool phase0 = (rank == 0) || (rank + 1 == legs.size());
const float ph = phase0 ? 0.f : PI;
angle = sin(stride + ph) * 0.32f;   // stride = gait phase (radians)
```

- **Diagonal pairs**: front-left swings with back-right, front-right with
  back-left. This is a walk, not a trot.
- The swing is about the leg's own axis across the body (`side`).
- The gait phase advances with **distance travelled**, not time, so feet keep
  rhythm when turning or slowing.
- A STOP freezes gait (no skating).
- Leg swing is **neutralised while grazing** (`angle *= (1 - grazeBlend)`): a
  grazing animal stands on four feet, it does not freeze mid-stride.

## Body motion

```cpp
const float sway = walking ? sin(gait) * 0.030f : 0.f;       // roll
const float bob  = walking ? sin(gait * 2.f) * 0.014f : 0.f; // vertical
const float pitch = graze * grazePitch;  // 0 while grazing (level body)
```

- **Bob is `sin(gait*2)`** (twice the stride) — the natural vertical
  oscillation of a walk.
- **Sway is `sin(gait)`** — side-to-side weight shift.
- **Graze keeps the body level**: the spine does NOT bow (grazePitch = 0).
  A spine bow tips the body and lifts the back feet off the ground. With rigid
  legs (no knee bend) the body cannot drop without sinking the front feet.
  The head reaches down through the neck alone.
- **Head nod**: `0.06 * sin(gait*2)` — bobs with the stride.
- **Tail sway**: `0.22 * sin(gait + 1.1)`.

## Ground contact — no sliding, no sinking

The ground clamp is **pose-aware**: it transforms the 8 corners of the rest
solid AABB by their nearest bone + the behaviour rotation, then lifts the body
until the lowest *posed* hoof clears the terrain:

```cpp
// For each corner of the rest solid AABB:
glm::vec3 posed = boneRot[bn] * corner + boneXform[bn];
glm::vec3 world = rot * (posed - pivot) + pivot;
minY = min(minY, world.y);
// lift
const float bottom = offset.y + minY;
if (ground > bottom) offset.y += ground - bottom + 0.002f;
```

Rules:
- **Lift-only.** A dip is not a hole to fall into; an object on a platform
  keeps its authored height.
- **Max of the four corners**, so a slope swallows neither edge.
- The clamp runs AFTER `animateBones()` (transforms are offset-independent, so
  animating first is safe and needed for the pose-aware bottom).
- `+0.002 m` clearance prevents z-fighting with the terrain.

## The fur follows the skeleton

The fur cards are generated in the field's **local** frame and composited by
`fur_overlay.comp`. The behaviour transform is mirrored every frame
(`m_furPass.setWorldAnimation`). The per-bone animation is still missing: the
overlay maps an animated point back to the REST pose before sampling the
field, so the field's cells never move, while the cards are drawn in that rest
frame. Fix: the card carries its bone index, the fur table carries the bone
base, the shader applies the bone's forward rotation.

## Verification

- `VF_TEST_BEHAVIOUR_FREEZE="alpaca:dx,dy,dz:headingDeg:gait[:graze]"` pins
  the pose for reproducible screenshots. `VF_TRACE_RIG=1` logs bone roles,
  parents and posed tips.
- `VF_TEST_FUR_STAT=1` measures the bone-classification rule's stability
  across gait phases (walk seam drift).
- micro_check gate: visible, bright neutral material, non-rectangular silhouette,
  one object shadows another, world view pixel-identical with micro layers on/off,
  SVO backend draws them too.

## Common pitfalls

- **Spine = longer horizontal axis**, not longest axis (animals are taller
  than long).
- **Belly = percentile**, not minimum (legs stand inside the spine band).
- **Leg reaches the ground** (coarse coat hangs below belly; drop test alone
  reads fleece as limbs).
- **Bones are a chain**, not independent rotations (composition of
  pivot-rotations is not a pivot-rotation; the neck tears otherwise).
- **Leg swing neutralises during graze** (`angle *= (1 - grazeBlend)`).
- **Graze keeps the body level** (no spine bow; rigid legs cannot extend).
- **Ground clamp is pose-aware** (transform corners by bone + behaviour).
- **Gait phase advances with distance**, not time (no skating on turns).
- **A STOP freezes gait** (no skating in place).

Source: `src/voxel/micro_rig.cpp`, `src/voxel/micro_behaviour.cpp`,
`src/app/world/micro_objects.cpp`, `shaders/micro_overlay.comp`.
