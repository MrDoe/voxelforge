---
title: SVO & dense raymarch backends
tags: [render, svo, parity, vulkan, shaders]
sourceRefs: [shaders/svo_raymarch.comp, shaders/raymarch.comp, src/app/main.cpp, tests/visual_check.py]
lastReviewed: 2026-08-24
---

# SVO & Dense Raymarch Backends

Two interchangeable compute-backend shaders render the same physical landscape:

- `shaders/svo_raymarch.comp` — primary: chunk grid (16³) → per-chunk octree → 8³ bricks
  packed with `rgb+sdf` | `a+refl+rough+matId` (2 words/voxel). Material truth = per-voxel
  brick data via `brickAlbedo` / `brickReflectivity`.
- `shaders/raymarch.comp` — dense reference: 256³ SDF volume @ 0.4 m over the full 102.4 m
  world. Material truth = analytic `kMatRefl[getMaterialId(p)]`.

## Parity contract (easy to break)

- `--compare` asserts mean abs channel diff < 14/255 + |coverage delta| < 0.08 (`app/main.cpp:879`).
- Every shading change must land in BOTH files. The shared `shadeTerrain(vec3 p, vec3 rd, vec3 alb, vec2 rr)`
  body is byte-identical; both backends pass their own material lookup as `rr`.
- The per-file divergence is confined to: `sceneMap` (SVO `map()` vs dense `sdfAt()`),
  the albedo/refl-rough sources, and the water-reflection march (SVO `map`, dense `sdfAt`).
- Water plane y = −0.9, fog density 0.0012, `softShadow`/`skyColor`/`heightAt` must stay
  identical or `--compare` fails.

## Verification loop

- `ctest --test-dir build` → unit_tests + visual_check (3 canonical shots: hero/house/water @480×270;
  asserts coverage 3–97 %, black-in-silhouette < 5 %, blue sky probe).
- `./build/voxelforge --compare` → SVO vs dense diff.
- `./build/voxelforge --smoke 400` → perf. Baseline ≈ 11.9 ms avg (headless); PBR+AO pass adds ≈ 0.6 ms.
- Screenshots: `--shot out.ppm --backend svo --animtime 0 --cam X Y Z TX TY TZ`; PPM→PNG via ffmpeg.
  Represented by `tests/screenshots/hero_pbr.png` etc.
