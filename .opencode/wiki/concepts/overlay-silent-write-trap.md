---
title: Overlay Silent Write Trap — Test Harness Can Destroy Ignored State
tags: [overlay, live-edit, data-loss, VF_OVERLAY_PATH, VF_NO_OVERLAY, runtime_edits, harness, asset-integrity]
sourceRefs:
  - tests/live_edit_check.py
  - src/app/frame/run.cpp
  - src/app/world/world_layers.cpp
  - AGENTS.md
lastReviewed: 2026-10-06
---

# Overlay Silent Write Trap

A test harness that renders can silently destroy versioned-and-ignored state.
`assets/runtime_edits.vxw` is gitignored, so damage is invisible in
`git status` — and a "clean-env" run that strips ambient variables reverts to
the shared path without warning.

## Mechanism

`VF_NO_OVERLAY=1` suppresses only the **load** of `assets/runtime_edits.vxw`.
Any run that **stamps** (`VF_TEST_EDIT`, `VF_TEST_STROKE`, `VF_TEST_BRUSH`,
or an interactive brush stroke) still **writes** the overlay back to
`assets/runtime_edits.vxw` unless `VF_OVERLAY_PATH` is set to a different
file. The guard is per-call-site, not a default.

## Harness-level cause

`live_edit_check.py:render()` defaults to `VF_NO_OVERLAY=1` (line 143) but
never sets `VF_OVERLAY_PATH`. The per-voxel + undo/clear call sites
(lines 192, 642, 668, 686) DO set it — so the guard is per-call-site rather
than default. A clean-env run strips whatever ambient var was hiding that.

## Positive firing test (2026-10-06)

A clean-env `live_edit_check` run rewrote `assets/runtime_edits.vxw` from
64,907,610 B to 980,100 B (md5 `65d4be40` → `a18739a7`). The file is
gitignored and no in-tree backup existed. **Confirmed unrecoverable**: George's
`/tmp/opencode` snapshot was lost when the directory was recreated; a
whole-disk name search found only the damaged file and
`/tmp/opencode/tester/overlay.vxw`; a 60–70 MB size sweep found no candidate;
git never tracked it. The cause was an unguarded render in the script —
`VF_NO_OVERLAY=1` was set but `VF_OVERLAY_PATH` was not, so the stamping
test wrote back to the shared path.

## Durable fix

Make `render()` default `VF_OVERLAY_PATH` into `/tmp` so the guard is the
default rather than the exception. Also: CMakeLists' `gpu_selftest` runs
`--selftest` with NEITHER var set — it should export `VF_OVERLAY_PATH` too.
Safe today because `--selftest` never stamps — an accident waiting for a
feature, not a considered guard.

**Status (2026-10-06) — three verification levels.** `test-live-edit` run
end to end leaves `assets/runtime_edits.vxw` byte-identical (md5 `a18739a7`,
980,100 B before AND after): **the stamping path is FIXED AND PROVEN**, which
was the destructive case. `CMakeLists.txt`'s `gpu_selftest` now pins both vars
(`VF_NO_OVERLAY=1` + `VF_OVERLAY_PATH=…/build/gpu_selftest_runtime_edits.vxw`),
cmake reconfigure rc=0 and `VOXELFORGE_TEST_GROUPS=visual,surfel ctest -R
gpu_selftest` → 100 % passed — but that guard is only **plumbed, not proven**,
because `--selftest` has no stamp path to protect. Note: bare `ctest -R
gpu_selftest` SKIPS (group gate, exit 77) — `VOXELFORGE_TEST_GROUPS` must be
set.

**Corollary from the day/night A/B:** a *copied* `assets/` still ships
`runtime_edits.vxw`, so a render against the copy rewrites the copy's overlay
unless `VF_OVERLAY_PATH` is set per-arm. The in-tree `render()` default covers
the test suite; a hand-rolled A/B script must set it itself.

**Status (2026-10-06):** the `render()` default-VF_OVERLAY_PATH fix is
landed and verified — env-level (fake binary across four cases: default
stamping → tmp path; explicit caller path wins; overlay=True keeps its own;
ambient env honoured) and real-suite (`live_edit_check.py --only preview`
PASSED with `assets/runtime_edits.vxw` md5 unchanged at `a18739a7`). Still
owed: a stamping render under `test-live-edit` (the ad-hoc `VF_TEST_EDIT`
at world 0.05,2.05,3.05 hit no surface and wrote nothing).

## Rule

> **Never treat `VF_NO_OVERLAY` as a read-only guard.** Export
> `VF_OVERLAY_PATH=/tmp/opencode/<name>.vxw` for EVERY scratch render.

`VF_NO_OVERLAY=1` alone is safe only for non-stamping renders (`--shot` without
edit hooks, `--selftest`, `--smoke`). Any render that can stamp must set
`VF_OVERLAY_PATH` to an isolated file.

## Related

- [[concepts/authored-assets-are-build-outputs]] — the other "your assets are
  not what git thinks" page (build outputs registered as source-tree files).
- [[entities/live-edit-brush]] (~lines 393-409) — the overlay persistence
  mechanism and the env vars.
- [[concepts/sun-direction-pipeline]] — the sun-manifest lane that also
  silently changes reference shots.
