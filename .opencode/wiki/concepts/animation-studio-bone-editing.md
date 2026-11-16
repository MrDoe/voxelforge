---
title: Animation studio — posing a rig and editing its bones
tags: [studio, animation, rig, gizmo, keyframes, micro-voxel, alpaca, ui]
sourceRefs: [src/app/ui/panel_animate.cpp, src/app/ui/bone_gizmo.cpp, src/app/ui/scene_overlays.cpp, src/app/frame/run_input.cpp, src/app/frame/run_hooks.cpp, src/app/app.hpp, src/voxel/micro_clip.cpp, src/voxel/micro_clip.hpp, src/voxel/micro_rig.hpp, src/voxel/fur_cards.hpp, src/render/micro_svo_pass.cpp, src/render/fur_pass.cpp, tests/test_micro.cpp, AGENTS.md, docs/tooling.md]
lastReviewed: 2026-11-16
---

# Animation studio — posing a rig and editing its bones

`--anim --anim-layer micro/alpaca.vxw` (or `./animation_editor.sh`) opens the
studio in the sidebar's **Animate** section (`Ctrl+7`). It targets ONE micro
layer, resolved **by name** into `m_studio.targetIndex`; the panel, the
viewport overlay, the clip clock and the camera all go through that resolved
index. The old "index 0" binding drove the VASE while the header said
`alpaca`. Locomotion is frozen in studio mode, so the animal stays put while
the clip still drives its bones.

## Two different data models, and it matters which one you are touching

| | lives in | edited by |
|---|---|---|
| **POSE** | the clip: `BoneKey { t, rotDeg }` per bone track | ring drag / pose fields, auto-keyed at the playhead |
| **RIG** | `MicroBone { a, b, radius }` | `length`, `joint X/Y/Z`, the gizmo's centre dot, the radius slider |

A pose is a **local rotation per bone**, composed down the chain
(`composeBoneChain`: `R_w = R_p·R_l`, `t_w = R_p·t_l + t_p`). The rig is
GEOMETRY: the segment the weight kernel measures against and the skeleton the
overlay draws. Conflating them is how you get "the pose looks right but the
body does not follow".

## One interpolation rule

`sampleTrackEuler` (`src/voxel/micro_clip.cpp`) owns the whole blend: clamp at
both ends (a lone key holds for the whole clip, never extrapolate) and
interpolate the bracketing pair with the STARTING key's easing. `MicroClip::`
`sample` poses the rig with it and the studio's readout displays it, so the
editor cannot hold a second opinion about the curve. The panel shows
`Interpolated at t=… (no key here)` between keys and auto-keys on edit,
seeded from the blend just shown.

Keys are quantised to `1/fps`, so one drag updates ONE key instead of writing
a key per mouse-motion event.

## The gizmo rotates about the bone's OWN axes

Three rings at the joint, **each the projected circle of that bone's local
X/Y/Z** (centre at the joint, normal along the axis, radius a fraction of the
bone's length). Hit-testing is point-to-projected-polyline.

The object trackball's fixed screen shapes (circle / wide ellipse / tall
ellipse) are **not** usable for a bone: a fixed screen ellipse claims a plane
the bone does not have, so which ring "was" X depends on the camera and the
drag rotates about a different axis than the one drawn. That was the first
version, and it read as wrong orientations.

The drag intersects the pointer with the ring's **plane** and tracks its
**angle** there; the delta is composed as `R · axisRotation(localAxis, Δ)` and
decomposed back into `rotDeg`. Composing on the LEFT would rotate about a
different axis — a unit test pins that `R·axisRotation` leaves `R·e` fixed and
that a full turn is the identity.

The Euler pair `boneEulerMatrix` / `boneEulerFromMatrix` is an exact inverse
pair, and `sample` builds from the same function, so the round trip is
testable. **`rotDeg.x` enters the matrix as-is**: the matrix pattern already
encodes the handedness, and negating it (a minus sign that belongs to the
reciprocal derivation) spins every X pose the wrong way.

## Moving a joint resizes implicitly

- **`length`** moves the TIP along the bone's own direction; the joint stays.
- **The joint** moves via the gizmo's centre dot (slides in the camera plane,
  world-per-pixel at the joint's depth) or `joint X/Y/Z`. The TIP stays put,
  so length AND direction change implicitly — there is deliberately no
  "translate the whole segment" mode, because rigidly moving a bone and then
  resizing it is two edits for one intention.
- A parent whose tip **was** that joint (within 1e-4 m) stretches with it, so
  a connected chain stays connected instead of tearing.

## The upload contract, and why it is the real work

Bone segments and radii are uploaded ONCE, when the world and fur tables are
built; only rotations stream per frame. So every rig edit MUST run
`studioUploadRig()` → `MicroSvoPass::setBoneSegments` +
`FurPass::setBoneSegments`. Without it the drawn skeleton and the CPU rig move
while the BODY keeps the old capsule — an editor that lies about what it
edited. The fur keeps its own copy in the layer's frame
(`FurObject::rigFrame`), so the conversion does not need re-deriving.

The same upload is what makes the influence-radius slider real; it used to
change the CPU rig only.

## Traps, each of which cost a debugging round

- **`ImGui::GetIO().DisplaySize` reads back NEGATIVE** before the first
  `NewFrame`, and a headless `--shot` may never build one. A projection with a
  negative aspect puts every gizmo off-screen (symptom: `centrePx ≈ (-0.5,-0.5)`,
  `radiusPx ≈ 0.1`). `App::viewportLogical()` falls back to the requested
  surface size.
- **The studio does not frame the camera.** It only tracks the target's
  movement; `m_camera` keeps the `--cam` value, so a headless studio shot must
  pass its own camera or the animal is off-screen.
- **At REST the skinning is the identity**, so a rig edit is invisible until
  the bone is posed: a leg shortened 0.50 → 0.12 m moves 3,004 px when rotated
  40°, but only 674 px at rest.
- **A drag must survive a rig reload.** The dragged bone is tracked by its
  stable **id**, not its index; a reload that reorders bones abandons the drag
  instead of writing a key onto the wrong bone.

## Verification without a mouse

A UI feature only a mouse can reach cannot be gated, so the drag is split into
`studioBeginBoneDrag` / `studioApplyBoneDrag` (and the translate pair), which
the frame loop and the headless hooks call.

| hook | measured on the shipped alpaca |
|---|---|
| `VF_TEST_BONE_DRAG="0:x:30"` | key `(30,0,0)`, tip `(-6.316,0.130,19.206)` |
| `VF_TEST_BONE_DRAG="0:y:30"` | key `(0,30,0)`, tip `(-6.040,0.406,19.206)` |
| `VF_TEST_BONE_DRAG="0:z:30"` | key `(0,0,30)`, tip UNCHANGED (the spine points along its own Z) |
| `VF_TEST_BONE_LENGTH="1:0.12"` | leg 0.5016 → 0.1200 m, posed length 0.1200 |
| `VF_TEST_BONE_MOVE="7:0,-0.10,0"` | head 0.1114 → 0.1973 m, tip moved 0.00000, parent 6 tip moved 0.10000 |

Each drag result is the exact right-handed rotation about that bone's axis —
the Z row is what proves the gizmo is axis-correct rather than merely
"rotating something". `VF_TRACE_RIG=1` prints roles, parents and posed tips.

See also [[concepts/micro-animation-cpu-cost]] (the bone maths is 0.1 % of a
frame, so the CPU is not the problem), [[concepts/fur-cards]] (the coat reads
the same bone table) and
[[concepts/geometry-format-policy]] (which path draws the object at all).
