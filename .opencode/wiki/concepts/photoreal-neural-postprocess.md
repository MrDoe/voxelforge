---
title: Reaching DLSS-5-class photorealism
tags: [rendering, post-processing, neural-rendering, dlss, photorealism, gbuffer, plan]
sourceRefs: [shaders/splat.frag, shaders/post.comp, src/app/frame/record_fx.cpp, src/render/taa_pass.cpp, shaders/motion_blur.comp, shaders/common_base.glsl, assets/world.json]
lastReviewed: 2026-11-18
---

# Reaching DLSS-5-class photorealism

**Question (2026-11-12):** can we make the frame look photorealistic "like with DLSS 5"?
**Short answer:** not by buying DLSS 5 — it is a closed, RTX-50-only, per-title-tuned
model with no drop-in path for a bespoke Vulkan renderer. The *look* it sells is a short,
named list of effects we can implement classically, and the one thing DLSS 5 genuinely
requires that we do **not** have is a proper guidance G-buffer. Fix the G-buffer first:
it is the prerequisite for the open ML options *and* for half the classical work.

## What DLSS 5 actually is (Sept 2026, shipped in NBA 2K27)

DLSS 5 adds **3D-Guided Neural Rendering**: a compact neural network that runs as the
**final stage of the graphics pipeline**, taking the engine's frame *plus its engine data*
and regenerating the lighting and material response. Per NVIDIA's own description it is
trained to recognise **colour, surface albedo, detailed lighting, surface normals and
motion vectors**, and it adds subsurface scattering, light transmission through hair and
foliage, deeper contact shadows and global illumination. Three properties are load-bearing
and are the reason it is not a filter:

- **Deterministic** — same input frame, same output. Not a diffusion model.
- **One-frame-in / one-frame-out** — temporal stability comes from *engine* motion vectors,
  not from a video model looking at a clip.
- **Art-directable** — multiple model weights, **Structure Intensity** (HF: AO, reflections,
  SSS) and **Tone Intensity** (LF: broad lighting/colour), plus semantic auto-masking and
  engine-level per-prop masking.

Hardware: **GeForce RTX 50-series only** (Blackwell Tensor Cores), up to 4K. Integration is
via **NVIDIA Streamline** or the **UE5 plugin**. NVIDIA also states the output quality
scales with input fidelity — richer source data gives dramatically better results.

**Consequences for us.** There is no `world.json` flag that turns it on. Streamline/UE5
both assume an engine's existing guidance buffers and (for Streamline) a D3D12-first
integration; we are a bespoke Vulkan compute/raster pipeline with **no albedo lane and no
motion vectors** (below). And DLSS 5 is tuned *per title* by its developers — an off-the-
shelf "make voxels photographic" model does not exist and would not exist for our art
direction even if the SDK were open.

## What we already have

The photoreal stack is bigger than it looks. `App::recordPhotorealism`
(`src/app/frame/record_fx.cpp`) runs, in order, **SSR** (bit 5) → **SSAO** (bit 6, two-band,
world-scale) → **volumetric fog** → **motion blur** → **depth of field**; `post.comp` does
HDR **bloom** and **AgX tonemapping with three looks** and exposure; `detailNormal`
(bit 7 / `B`) adds a Sobel-of-luminance bump; the photo atlas + triplanar sampling gives real
albedo; fur cards give a translucent coat; point lights and emissive-derived lights exist.

Two things from that list matter for this discussion:

1. **The whole FX chain runs in-place on the *already-tonemapped LDR* `m_offscreen`.**
   `post.comp` tonemaps, then SSAO/SSR/fog/blur multiply a compressed range. A contact
   shadow cannot get deeper and a specular highlight cannot get brighter, because the
   headroom is gone. DLSS 5 works on the pre-grade frame — so does every real
   photographic pipeline. This is an **ordering defect**, not a taste difference.
2. **TAA is OFF by default and never runs headless** (see
   [[concepts/render-defaults-off]]). Every reference shot we make is the *un-stabilised*
   frame. A neural refiner, and the human eye at 4K, both assume a clean AA'd base.

## The blocking gap: no guidance G-buffer

**Status (2026-11-18):** steps **0, 1 and 3 landed** in commit `166e490` (all
default-off flags, shipped look untouched); only **step 2 (velocity buffer)**
is still missing. Backlit fleece SSS (`VF_FUR_SSS`, `62378ae`) and the 64³
irradiance volume also landed. The tables below keep the original plan for
context — the "Unlocks" column now describes what the landed flags expose.

`shaders/splat.frag` writes **five** attachments (three original + two guidance
lanes added in step 1):

```
layout(location = 0) out vec4 oHdr;    // final shaded colour
layout(location = 1) out vec4 oGPos;    // world xyz, w = hitType
layout(location = 2) out vec4 oGNorm;   // shading normal
layout(location = 3) out vec4 oGAlb;    // EFFECTIVE albedo (post-triplanar, pre-flora), a = roughness
layout(location = 4) out vec4 oGLit;    // shading BEFORE atmosphere composite, a = reflectance
```

Everything the inputs of a neural pass want is **present but destroyed inside shading**:
`vMat` carries `mat, refl, rough, ao` and `vTex` the texture override, and triplanar albedo
is sampled and immediately multiplied into the lighting integral. So we have no albedo
image, no roughness image, no separate direct/indirect split — and therefore nothing to
condition a model on, and nothing to feed DLSS-SR/RR or XeSS either.

Worse on the temporal axis: **there is no velocity buffer.** `motion_blur.comp` and
`taa_resolve.comp` reconstruct velocity *analytically* by reprojecting `oGPos` with the
**previous camera** (`m_prevCam`, `App::advancePrevCam`). That yields the correct field for
camera motion and **zero for object motion**. A walking alpaca, a live-rotated layer, a
carved chunk and a dragged selection all have no velocity, which is exactly why TAA is
interactive-only for us and why a walking shot is not A/B-deterministic
([[concepts/render-nondeterminism]], [[concepts/micro-animation-cpu-cost]]).

So the honest sequencing is:

| Step | Work | Status | Unlocks |
|---|---|---|---|
| **0** | Move the FX chain **before** tonemap; keep `m_offscreen` linear HDR, tonemap last | **LANDED** — `VF_LINEAR_FX=1` runs the chain on `m_hdr` (rgba16f, matching the shaders' declared format) before `post.comp`; `m_linearFx` | all of the below looks right |
| **1** | Two extra MRT lanes in `splat.frag` | **LANDED** — `oGAlb=(effective albedo, roughness)` and `oGLit=(pre-atmosphere lighting, reflectance)`, both rgba16f; gated by `tests/gbuffer_check.py` + `VF_GBUFFER_DUMP=<prefix>`; tile path still drops them | classical relighting, ML input, DLSS-SR/RR/XeSS input |
| **2** | A real **velocity buffer**: gpos delta minus camera-only reprojection (owner transform `RotUBO`, micro behaviour, store edit AABB) | **MISSING** — `motion_blur.comp`/`taa_resolve.comp` remain camera-only analytic | TAA/blur on moving objects, temporal stability, any ML pass |
| **3** | Auto-exposure (downsampled log-luma histogram + adaptation) | **LANDED** — `ExposurePass`, `shaders/exposure.comp` (64-bin histogram, two dispatches, SSBO read by `post.comp` binding 4), `VF_AUTOEXPOSE=1` + render-flag bit 10 | cave↔exterior, the single biggest "photo" cue we lack |

Step 1–2 are cheap and pay for themselves with no model involved. Steps 0–3 are the
prerequisite work; **do not train or integrate a model before them**, because NVIDIA's own
stated law (output quality ∝ input fidelity) applies to a self-trained model ten times
harder than to DLSS.

## What to build instead of DLSS 5, ranked by look-per-effort

Our own measurement history says **sharpening is not the lever** — see
[[concepts/texture-resolution]]: doubling atlas resolution bought **+4.8 %** near-field
Laplacian energy, while detail normals bought **+40 %** and fog restructured depth cues.
Ranking anything DLSS-5-"Structure" by the same yardstick:

1. **Foliage/fleece transmission (backlit translucency).** DLSS 5's signature cues are
   "light through ears and hair". We already have `applyFlora` and fur cards; a wrap
   diffuse + backscatter lobe on the flora and fleece materials is a few lines and is the
   most photographically-legible thing on this list.
2. **A bounce of real GI.** The world is sun + analytic sky ambient + baked binary shadow.
   A voxel world makes irradiance caching nearly free: we already bucket emitters on a
   **1 m grid** (`VoxelField::collectEmissive`) and own a height texture plus a 256³ object
   volume. A coarse irradiance/probe grid on that same lattice gives colour bleed (green
   grass onto a cream wall) — the tell that a frame is "rendered" rather than photographed.
3. **Roughness variation.** Real wood, wet rock and thatch are *not* one roughness. The
   photo textures already carry the variance; sample it into `vMat.z` instead of a constant
   per material. This is what makes specular read as a material instead of a sheen.
4. **Contact shadows that fall off with distance.** Our baked sun shadow is *binary* and
   SSAO is a two-band screen-space term. What the eye reads as photographic is the
   penumbra growing with height — a small distance-weighted AO term, not more samples.
5. **Camera grade:** auto-exposure (step 3 above), a wider/filmic bloom, subtle chromatic
   aberration and grain. Cheap, and the last 10 % of "that looks like a photo".
6. **Enable TAA (with subpixel jitter) for anything meant to be looked at.** Every gate
   currently measures the noisy frame.

### And if we actually want a neural pass

- **DLSS-SR / RR / FG via the DLSS SDK on Vulkan** is the only *literally* DLSS option: it
  is a downloadable SDK, works from Vulkan, and needs the jittered projection matrix plus a
  velocity buffer (steps 1–2). It gives resolution and denoising, **not** the DLSS 5
  lighting look, and it is RTX-only.
- **Intel XeSS** ships open-source with a Vulkan plugin and a DP4a path for non-Arc GPUs;
  **AMD FSR 3.1+** upscaling is open and hardware-agnostic. Both are upscalers, not
  relighters — same honest limit.
- **Offline, for the hero frame only:** run our own G-buffer (steps 1–2 make this possible)
  through a local depth+normal-conditioned image model over Ollama/ComfyUI. This *is*
  DLSS-5-shaped, and it is fine for a still. It is not fine interactively: it is
  non-deterministic frame to frame, which fails the property that makes DLSS 5 usable at
  all, and it fails every pixel gate in `tests/`.
- **A self-trained refiner** (small distilled UNet, quarter-res, ONNX-Runtime-Vulkan or a
  hand-written Vulkan-compute port) trained on pairs of *our* frames against a
  ray-traced/`--mode svo` reference. Real, and the only route to a model tuned to voxels —
  but it is a research project, and note the trap: our SVO backend is the pixel reference,
  so training on it teaches the model to reproduce the *same* material simplification, not
  to exceed it.

## Bottom line

Do the ordering fix, the albedo/roughness lanes and the velocity buffer. That is
non-negotiable ground-work for every option above and improves the frame on its own. Then
take the ranked classical list — transmission, one GI bounce, roughness variation,
distance-aware contact shadows, auto-exposure. That gets the DLSS-5 *look* with zero
closed-source dependencies and stays inside the gates. A neural stage becomes worth
considering only once the guidance buffers exist, and only as an RTX-50 SDK feature or an
offline still.

Cross-refs: [[concepts/texture-resolution]], [[concepts/detail-normals]],
[[concepts/render-defaults-off]], [[concepts/enclosed-space-lighting]],
[[concepts/fur-cards]], [[concepts/shading-model]], [[concepts/volumetric-fog]].
