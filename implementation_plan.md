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

## **6. Optional Extensions**

You may add:

- soft shadows
- multi‑light support
- temporal smoothing
- hierarchical proxy structures

All extensions must preserve the minimal‑raytracing philosophy.

