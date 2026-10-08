---
title: Focused test groups
tags: [testing, ctest, cmake, workflow]
sourceRefs: [CMakeLists.txt, tests/group_gate.py, docs/testing.md, AGENTS.md]
lastReviewed: 2026-10-03
---

> **Post-split (2026-09-26).** `src/app` was split per subsystem; `main.cpp` is
> now a 16-line entry point. All `main.cpp:<line>` references formerly on this page
> have been re-anchored to **symbols**. Use [[entities/app-subsystems]] for the map and
> for the two invariants that straddle files — the click gate's write side is in
> `edit/live_edit.cpp` and its read side in `frame/run_input.cpp`.

# Focused test groups

Voxelforge tests are opt-in functional groups. There is intentionally no
all-tests target and a bare `ctest --test-dir build` does not start any test
body: every CTest command is wrapped by `tests/group_gate.py` and returns CTest's
skip code unless its group is enabled.

## Groups are independent — a red in one never skips another

Worth stating because it is easy to assume the opposite, and the assumption costs
a day. Each `test-<group>` target is `ctest -L <group>` with
`VOXELFORGE_TEST_GROUPS=<group>`, and `vf_add_test_group` gives it `DEPENDS` on
the **binaries only** (`vf_add_test_group(night voxelforge)`). There is no
`DEPENDS`, no `FIXTURES_REQUIRED`, and no ordering between groups.

So `test-visual` being red **cannot** skip `test-night`. Verified 2026-10-08
after a session recorded `test-night` as "skipped when the visual group went
red" — the two are unrelated, and the night gate had simply not been run. The
two `test-visual` reds in that window were the **known pre-existing** house/water
sky-probe failures, which are camera assertions on correct content
([[concepts/sky-probe-is-a-camera-assertion]]) and have no bearing on night.

**Practical consequence:** when a gate is reported as skipped, check whether it
was *run* before accepting the reason. "Skipped" and "not run" are different
states, and only one of them is evidence.

Related, and the reason a group can be trusted at all: **run groups sequentially
on a shared GPU** — see below.

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
| `test-night` | night/day ratio band, night sky classifier, `kMoonCol` tripwire (`night_check.py`) |
| `test-smoke` | all fast variants; `test-fast` is a compatibility alias |
| `test-preview` | brush preview only: tint hue, Depth sensitivity, SVO parity (`--only preview`) |

**`test-preview` exists because "focused" was not focused enough (2026-10-01).**
The brush-preview visibility fix touched `shaders/post.comp`,
`shaders/svo_raymarch.comp`, `common_surfel.glsl` and the frame's preview
plumbing, which by the table above meant `test-live-edit` *or* `test-visual` —
both dominated by per-run world loads (~13 s each) for checks that are really
one frame each. `live_edit_check.py --only preview` renders its own baseline and
runs just `check_preview` + `check_depth_sensitivity` + `check_svo_preview`, so
a preview or post-pass tweak no longer implies the whole live-edit matrix. It
stays behind the same `group_gate.py` (`SKIP_RETURN_CODE 77`) and is *not* part
of any other group: `test-live-edit` still runs the full suite, preview checks
included, so the split adds a cheap entry point without weakening the gate.

The general lesson matches the coverage-gap note below: a gate that can only be
run expensively tends to simply not be run after a small change.

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
(in `frame/run.cpp`, right after the `headless submitted` trace), so the probe is
reachable from headless *because* of the fix,
and `--shot` shares that path so it reaches it too (measured: 1 line, correctly
latched across its 3 frames). Now that it reads correctly, the interesting part
is *why `--smoke` is still used*, and it is **not** reachability: `--smoke` runs
enough frames for "once per distinct code" to **mean** anything, whereas over
`--shot`'s three frames the latch would pass **even if it were broken**. Switching
to `--shot` would look identical in the source and silently destroy the property
the check exists to pin. Both facts are in the comment, and the reachability one
is attributed to the bug history rather than presented as a constraint. Generalised
on [[concepts/measurement-discipline]].

### Building a render gate that can actually fire

The splat backend is **not bit-reproducible**: the same unchanged binary rendered
twice gives different hashes, so a `sha256` render gate is an instrument that can never
pass or fail. The working shape is **statistics within a measured noise floor**,
calibrated against a **pristine-HEAD control binary built specifically so the
comparison is not assumed**.

What the noise actually looks like, measured on a 640×360 hero shot (reusable when
writing the next render gate):

| statistic | behaviour across runs |
|---|---|
| mean RGB | **exact** |
| p99 | **exact** |
| p50 of per-pixel diff | **varies** — observed −0.009 … +0.022 against a floor of 0.03 |

So the instability is confined to the *distribution* of per-pixel differences, not to
the summary statistics — pick a gate statistic accordingly, and calibrate the floor
from the control rather than picking a round number.

**Run groups sequentially on a shared GPU.** A `test-live-edit` red was traced to four
concurrent instances competing for the device; alone it was 3/3.

**`ninja -k1` stops scheduling after the first failing target — so downstream
targets never start, and it reads as "skipped".** This is ninja-level, not
ctest-level: the groups are independent (see above), so a red in one group
cannot skip another. A `ninja -k1` pass that dies on `test-textures` leaves
`test-night` never-started, which gets recorded as a skip with a confident
wrong story about why. **"Skipped" and "not run" are different states, and only
one of them is evidence** — check whether a gate was actually run before
accepting the reason. Use `-k0` to keep going past failures when the full
picture is wanted.

**Refined 2026-10-03 — a contended measurement is VOID, not "provisionally green".**
The earlier wording ("a red under concurrency is a contention hypothesis") quietly
allowed a contended PASS to be banked, which is the more expensive error: it gets
believed later. Contention flips a pixel-diff verdict in **either** direction — the
same check has measured **137.9 s PASS vs 198.4 s FAIL** on one binary — so the rule is
*discard and re-run on a confirmed-quiet machine*, never "distrust failures".

Timing is the **tell**, never the evidence: judge by `pgrep`, not by whether a number
looks fast. Measured fingerprints (uncontended → contended):
`unit_store_tests` 37 → 96 s, `fast_live_edit_check` 23 → 78 s,
`unit_surfel_tests` ~85 → 150 s, `preview_check` 136.6 → 406.8 s.

One session voided its own `test-preview` PASS only after the runtime gave it away, and
voided a `test-surfel` 4/4 for the same reason. Two renderers coexist easily in this
repo — a bare interactive `./build/voxelforge` (possibly the **user's**, holding a large
`assets/runtime_edits.vxw`; never kill it, never `rm` that overlay) or another agent's
GPU group chained into a backgrounded command. Always run
`pgrep -af "build/[v]oxelforge"` immediately before trusting a diff, and always
`VF_OVERLAY_PATH=/tmp/…` for scratch renders.

**Known gap: interactive UI has no automated coverage at all.**
`App::drawHud()` → `drawSidebar()` is called from `record_interactive.cpp` **only**, so
no headless gate (`--shot`/`--shotlist`, hence every `test-<group>`) ever draws the
sidebar or any panel. Two independent sessions each landed new panel UI on 2026-10-03
(the brush Falloff curve combo + `f(q)` plot; the edit-mode hotkey bar) with zero
coverage for this single reason. A build error is caught; an ImGui assertion in panel
code is not — and this repo has already been bitten by exactly that (an unguarded
`TableSetColumnIndex` after a clipped `BeginTable` segfaulted at 960×540). Reaching a
non-default panel section headlessly is also not possible as it stands (no `xdotool`;
the default section is `Panel::World`). Cheapest fix, if wanted: a `VF_TEST_PANEL=<section>`
hook that sets `m_panel` and lets the shot path call `drawHud()` once. Until then, treat
new panel/overlay UI as **manually-verified-only** and say so rather than implying it is
covered.

The CMake target sets `VOXELFORGE_TEST_GROUPS=<group>` and selects the matching
CTest label. Individual entries retain `ctest -N`/`ctest -L` discoverability,
but direct execution still requires the group environment.

Choose the smallest group that covers the changed files. Add a second group
only when the change crosses a boundary (for example, a surfel/live-store
change uses `test-surfel`; a shader/visual change uses `test-visual`).
`test-surfel` and `test-visual` include the GPU `--selftest` acceptance check.

See [[concepts/edge-aware-surfel-radius]] and [[concepts/detail-pipeline]] for
the current surfel changes that use this workflow.
