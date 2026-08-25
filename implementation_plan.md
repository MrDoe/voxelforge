# **Minimal‑Raytracing Splat Renderer — Implementation Guide**

A complete, real‑time shadow pipeline for **Gaussian Splats** using **minimal raytracing**.
Fast, stable, and fully compatible with dynamic light sources.

---

## **1. System Overview**

This renderer combines three components:

1. **Billboard Gaussians** for the main image
2. **Minimal raytracing** (one ray per splat)
3. **Shadow factor darkening**

This avoids:

- shadow maps
- volumetric raymarching
- secondary splat passes
- expensive light‑space projections

---

## **2. Pipeline Steps**

### **Step 1 — Generate Billboard Gaussians**

Each splat must contain:

- 3D position
- radius
- base color
- density
- billboard sprite facing the camera

This is the standard Gaussian‑Splat rendering setup.

---

### **Step 2 — Minimal Raytracing (One Ray per Splat)**

For each splat:

- Cast **one ray** toward the light source
- Test the ray against a **proxy structure**
- Output: `free` or `blocked`

#### **Proxy Structure Options**

- 64³ or 128³ voxel grid
- BVH over splats
- Signed distance field

The ray never tests against all splats — only the proxy geometry.

This keeps raytracing extremely fast.

---

### **Step 3 — Compute Shadow Factor**

If ray is blocked:

```
s = 1
```

If ray is free:

```
s = 0
```

Optional soft‑shadow modes:

```
s = smoothstep(d)
```

or

```
s = occlusion_density
```

---

### **Step 4 — Apply Shadow Darkening**

In the main rendering pass:

```
color_final = color_base * (1 - s)
```

This is cheap, stable, and works with millions of splats.

---

## **3. Compact Pipeline Table**

| Step | Purpose | Technique |
|------|---------|-----------|
| **Generate Splats** | Scene representation | Billboard Gaussians |
| **Proxy Raytracing** | Shadow visibility | One ray per splat |
| **Shadow Factor** | Light occlusion | 0–1 scalar |
| **Darken Splats** | Apply shadows | Color multiplication |

---

## **4. Why This Works**

This pipeline avoids all expensive shadow techniques:

- no raymarching through millions of Gaussians
- no shadow maps
- no secondary splat rendering
- no volumetric integration

Instead, it uses **visibility rays**, similar to particle systems.

This yields:

- real‑time performance
- dynamic shadows
- stable lighting
- minimal GPU load

---

## **5. Performance Expectations**

On modern GPUs:

- **100k–1M splats per frame**
- **proxy raytracing in microseconds**
- **shadow darkening is free**

Suitable for:

- real‑time visualization
- interactive scenes
- dynamic lighting
- VR/AR applications

---

## **6. GaussianShader Integration (Tier 1 implemented, Tier 3 deferred)**

Tier 1 (real-time, no training) implemented `2026-08-23`:
- Flattened disks: `scale (sx=sy=spacing*1.25, sz=VOXEL*0.55)`, shortest axis `v = normal` (`Fig.4`), `n=±v` flip `Eq.5` with `Δn=0` in frag; foreshortening `mix(0.55,1,|N·V|)` in `splat.vert`.
- Shade `Eq.3` with `c_r=0`: `c=γ(c_d + s⊙Ls)`, `s,ρ` from `kMaterialReflection`, `Ls` via GGX-prefiltered HDR `6×64×64` cubemap (`Eq.4`, `lod=ρ*6`, `256` Hammersley samples per mip in `SplatPass::createEnvCubemap`), Schlick `F`, `aces`+gamma. Water variant `ρ=0.15` high specular.
- Denser `spacing 0.15m` (was `0.21`), water `0.22m`, budget `2.5M`, stride `4×vec4` `Splat{posRadius,albedoAO,normalMat,shadeParams}`.
- HDR `assets/env.hdr` loaded via `stb_image` equirect→cubemap, fallback procedural Hosek sky if missing.
- Residual SH kept `0` (training-needed; plumbing reserved for `vec4 shadeParams` → `+SH9` later).

Tier 3 (deferred, doc-only): differentiable training of `c_d,s,ρ,c_r(SH3),Δn1/2,env` via `tools/train_gs/` (`L = L_color+0.01L_normal+0.001L_sparse+0.001L_reg` `Eq.9`, `30k` Adam, `0.58h`), dataset capture `tools/capture_dataset --dump-train`.

## **7. Minimal Raytracing Shadows — Implemented 2026-08-24**

**Status: ✅ Implemented** — one visibility ray per splat against SDF proxy (heightmap + `scene()`), no shadow maps.

*   **Splat data:** `SplatVertexData::shadow` `float 0..1` (0 lit, 1 shadowed) packed as `vec4` in `Splat{posRadius,albedoAO,normalMat,shadeParams,shadow}` stride `5×vec4` (`src/render/splat_pass.hpp:14`, `src/render/splat_pass.cpp:406` always `5` when `hasNew`).
*   **Proxy ray:** CPU `softShadow(ro+ N*0.06, sunDir)` 12 steps `res=min(res,9*d/t)`, `t+=clamp(d*0.85,0.05,1.2)`, `d=scene(sp).d` (`src/app/main.cpp:620` `buildSplatData` + `src/app/main.cpp: ~750` `initVulkan`), parallel `std::execution::par` via `tbb` (`CMakeLists.txt:140` `tbb`), toggle `H` / `--shadows` / `--noshadows` (`src/app/main.cpp:178` `m_splatShadows`, `src/app/main.cpp:1315`).
*   **Shading:** `splat.frag:28` `shadowDark=1-shadow*0.65`, `diffuse * shadowDark`, `specularEnv * shadowDark`, `spec * shadowDark` (`shaders/splat.frag:45`).
*   **Density-aware:** `earlyBudget` `1.5M/2.5M/5M/10M` for `1x/2x/4x/8x`, shuffled `voxOrder` for uniform, `SDF|d|<0.22` filter for subgrid, `waterSpacing=0.45/interp`.
*   **Performance:** `2.5M` `8.6s` with shadows (vs `5s` without) — `12` steps `par`; `H` rebuild via `rebuildSplats()` `vkDeviceWaitIdle`.

## **8. Optional Extensions**

You may add:

- multi‑light support (extend `shadow` to per-light)
- temporal smoothing (accumulate `shadow` over frames)
- hierarchical proxy structures (SVO/BVH already available as proxy alternative to SDF)

All extensions must preserve the minimal‑raytracing philosophy.

