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

**Coverage gap FOUND and now CLOSED (2026-09-26).** The live-edit checks used to
edit **terrain only** — no case touched an object chunk at all. That is how a
live-edit defect reached a user through a fully green `test-live-edit`. It was
not hypothetical: a 1-voxel add followed by its undo moved 0.18 % of pixels on
the add and **2.57 %** on the undo, while a separate terrain case moved 0.02 % —
so a terrain-only run sees essentially nothing and **cannot fail**.

*Provenance note.* This contrast was briefly quarantined because cell
`562,524,607` was believed to log a terrain pick. That was wrong — the reader
was broken, not the world (the headless hook never set the field). Re-measured
with the fixed instrument: the object cell and the terrain cell give 0.18 % and
0.02 % on the same one-voxel add, so the contrast stands. The lesson is on
[[concepts/measurement-discipline]].

The gap is now gated by `check_object_undo_surgical` in
`tests/live_edit_check.py`, in the **full** path (not the fast profile). It
asserts the margin class primarily by **true-inverse comparison** — the post-undo
run split must *equal* the add-only run split (4298 + 52 == 4298 + 52 against
4157 + 602), which has no threshold and cannot drift on re-author — behind three
premise asserts (the stamp happened, the region holds real baked geometry, and
the pick is an **object** cell) plus two secondaries (edges ≤ 200, undo pixel
diff ≤ 0.5 %). Every premise is backed by a **negative control**: an air
coordinate and a terrain cell each fail loudly, the terrain cell doing so
*silently* on the true-inverse comparison alone. The reasoning is in the check's
docstring, so it travels with the assertion. Root cause, and the generalisation that a wider re-derivation is not
a safer one, are on [[entities/live-edit-brush]].

**All three vacuity paths on that check are now closed by premise asserts**
(stamp happened; region holds real baked geometry; pick is an object cell), each
backed by a negative control — an air coordinate and a terrain cell. The earlier
concern that a frozen constant could leave the check silently inert is therefore
answered: a hardcoded cell remains a *staleness* risk to watch for, but it can no
longer pass vacuously without a premise firing. Details, and the two-step
correction this page went through, are on [[entities/live-edit-brush]].

### `check_present_probe` — a diagnostic that finally has a firing test

The acquire/present result probe (HUD "window went dark" class of failure) had
**no positive firing test** for its whole life, so "it logs" was a claim nobody
could check — the textbook *silence is not a clean bill of health* case. It is
now driven by `VF_TEST_FORCE_PRESENT_ERR`, which feeds **synthetic** `VkResult`s
into the reporter so the thing under test is the *logger*, never the real
present. Belongs to the **visual** group, not live-edit: the probe is a frame-loop
path.

**The forced-failure test found a real bug in the existing probe.** The
log-once latch remembered only the **last** code, so it actually meant "log
whenever the code *changes*"; two failures **alternating** produced 60 lines in
30 frames — precisely the flood the latch exists to prevent. It survived review
because a real *persistent* failure repeats one code and so behaves correctly.
Now a bounded, saturating per-slot set (8), and the check asserts both severity
(device-lost / out-of-host-memory at `error`, the rest at `warning`) and
**one line per distinct code**, using a repeated code in the list to make the
latch observable.

**The hook had to move to be reachable at all** — see
[[concepts/measurement-discipline]] for why a path only the windowed loop can take
is invisible to coverage while looking correct in source.

**Docstring corrected (2026-09-26).** An earlier version of the check's
docstring said *"needs `--smoke`, not `--shot` … the probe is only reachable from
the real frame loop"* — the **pre-fix** reasoning, restated as a live requirement,
and wrong twice: `forceFrameResults()` is called from the **headless** frame body
(`main.cpp:5367`), so the probe is reachable from headless *because* of the fix,
and `--shot` shares that path so it reaches it too (measured: 1 line, correctly
latched across its 3 frames). Now that it reads correctly, the interesting part
is *why `--smoke` is still used*, and it is **not** reachability: `--smoke` runs
enough frames for "once per distinct code" to **mean** anything, whereas over
`--shot`'s three frames the latch would pass **even if it were broken**. Switching
to `--shot` would look identical in the source and silently destroy the property
the check exists to pin. Both facts are in the comment, and the reachability one
is attributed to the bug history rather than presented as a constraint. Generalised
on [[concepts/measurement-discipline]].

The CMake target sets `VOXELFORGE_TEST_GROUPS=<group>` and selects the matching
CTest label. Individual entries retain `ctest -N`/`ctest -L` discoverability,
but direct execution still requires the group environment.

Choose the smallest group that covers the changed files. Add a second group
only when the change crosses a boundary (for example, a surfel/live-store
change uses `test-surfel`; a shader/visual change uses `test-visual`).
`test-surfel` and `test-visual` include the GPU `--selftest` acceptance check.

See [[concepts/edge-aware-surfel-radius]] and [[concepts/detail-pipeline]] for
the current surfel changes that use this workflow.
