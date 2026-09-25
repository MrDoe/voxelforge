---
title: Focused test groups
tags: [testing, ctest, cmake, workflow]
sourceRefs: [CMakeLists.txt, tests/group_gate.py, docs/testing.md, AGENTS.md]
lastReviewed: 2026-09-25
---

# Focused test groups

Voxelforge tests are opt-in functional groups. There is intentionally no
all-tests target and a bare `ctest --test-dir build` does not start any test
body: every CTest command is wrapped by `tests/group_gate.py` and returns CTest's
skip code unless its group is enabled.

## Group entry points

| Target | Covers |
|---|---|
| `test-unit` | broad doctest/CPU changes |
| `test-surfel` | surfel extraction, radius/geometry, fast visual/live checks, GPU selftest |
| `test-store` | ChunkStore foundations, rebuild invariants, fast live edit |
| `test-world` | layered world, SVO, worldfile/record doctests |
| `test-live-edit` | store/brush/live-patch paths and the full live-edit check |
| `test-visual` | camera, shader, scene acceptance, full visual check, GPU selftest |
| `test-effects` | SSAO and its fast variant |
| `test-textures` | atlas/material texture checks and fast variant |
| `test-fog` | volumetric fog checks and fast variant |
| `test-smoke` | all fast variants; `test-fast` is a compatibility alias |

The CMake target sets `VOXELFORGE_TEST_GROUPS=<group>` and selects the matching
CTest label. Individual entries retain `ctest -N`/`ctest -L` discoverability,
but direct execution still requires the group environment.

Choose the smallest group that covers the changed files. Add a second group
only when the change crosses a boundary (for example, a surfel/live-store
change uses `test-surfel`; a shader/visual change uses `test-visual`).
`test-surfel` and `test-visual` include the GPU `--selftest` acceptance check.

See [[concepts/edge-aware-surfel-radius]] and [[concepts/detail-pipeline]] for
the current surfel changes that use this workflow.
