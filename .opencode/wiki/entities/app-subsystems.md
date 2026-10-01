---
title: src/app subsystem map (post-split) and the two invariants that straddle files
tags: [app, architecture, refactor, symbols, invariants, gizmo, brush, build]
sourceRefs: [src/app/app.hpp, src/app/main.cpp, src/app/frame/frame.hpp, src/app/frame/run.cpp, src/app/frame/run_input.cpp, src/app/frame/run_startup.cpp, src/app/frame/run_hooks.cpp, src/app/frame/run_hotkeys.cpp, src/app/frame/record_headless.cpp, src/app/frame/record_interactive.cpp, src/app/edit/live_edit.cpp, src/app/edit/transform.cpp, src/app/rhi/present_probe.cpp, src/app/rhi/surface.cpp, src/app/ui/ui_types.hpp, CMakeLists.txt, AGENTS.md]
lastReviewed: 2026-09-26
---

# src/app subsystem map (post-split)

`src/app/main.cpp` was a ~5470-line monolith and is now a **16-line entry point**.
The application is one directory per subsystem with `src/app/app.hpp` as the root
header, and `frame/run.cpp` is the **only** file that knows the frame-loop order.

**Anchor pages to symbols, never to line numbers.** Every `main.cpp:<line>` citation
this wiki carried through the split is stale by construction; a line number is a claim
with an expiry date (see [[concepts/measurement-discipline]]). The verified map:

| area | files | notable symbols |
|---|---|---|
| root | `app.hpp`, `main.cpp` | `class App` + frame-slice declarations; entry point only |
| `cli/` | `args.{hpp,cpp}` | `parseArgs`, `Args`, `ShotSpec` |
| `rhi/` | `surface.cpp`, `present_probe.{cpp,hpp}` | `m_acquireSems`, `ensureAcquireSemaphores`; `reportFrameResult`, `forceFrameResults` |
| `world/` | `world_layers`, `world_textures`, `store_overlay`, `surfel_stream` | layered world, terrain/objvol uploads, runtime-edit overlay |
| `textures/` | `texture_bindings.cpp` | the atlas binding table |
| `edit/` | `live_edit.cpp`, `transform.cpp` | `applyEditLive`, `commitStoreEdits`, `finishStroke`, `undoEdit`, `clearLiveEdits`, `storeNormalAt`, `adoptPickOwnership`; `commitRotation`, `commitMove` |
| `mesh/` | `mesh_import.cpp` | STL/OBJ import |
| `ui/` | `ui_types.hpp`, `ui_primitives.*`, `gizmo_math.*`, `theme`, `sidebar`, `panel_{edit,world,render,textures,mesh,ai}`, `scene_overlays` | the one docked sidebar and its six sections |
| `frame/` | `run.cpp`, `run_startup`, `run_poll`, `run_input`, `run_hooks`, `run_hotkeys`, `run_brush_preview`, `record_headless`, `record_interactive`, `record_fx`, `selftest`, `profiler`, `frame.hpp` | the frame loop and its slices; both recording paths |

`CMakeLists.txt` lists every `.cpp` **explicitly — no `GLOB`** — so a new file that is
not listed there does not build, and a new file is invisible until it is added. The
sibling hazard is one directory over: `assets/` is registered as build *outputs*, so
[[concepts/authored-assets-are-build-outputs]].

## Two invariants that deliberately straddle files

These are the things a reader carrying the old monolith map will get wrong, and
neither is visible from any single file.

**1. The click gate is split across two subsystems.** `m_lastStampWroteCell` is
**written** in `edit/live_edit.cpp` (set in `applyEditLive`, cleared in `undoEdit`)
and **read** in `frame/run_input.cpp` for the identity test that stops a held click
stacking. The state belongs to the edit path; the decision belongs to the input path.
A description of the click-vs-drag gate that names only the read side describes half
of it — see [[entities/live-edit-brush]].

**2. The 60° FOV is shared, not a loop-local.** `tanHalfFov` used to be a local inside
`App::run`, which is *precisely* why the UI/gizmo slices could not reach it. It is now
`tanHalfFov60()`, defined in `frame/frame.hpp:37` and read at **four** call sites, all
in `frame/`: `record_interactive.cpp`, `record_headless.cpp`, `run_brush_preview.cpp`
and `run_input.cpp`. So the gizmo/input hit-test, both recording paths and the brush
preview all read one function instead of each keeping its own copy of the constant, and
the *enabler* behind the gizmo-hits-agree-with-push-agreements invariant is that
sharing rather than two copies agreeing by coincidence.

*(An earlier draft of this page said "five call sites" and "three subsystems". Both
were wrong: the fifth was the definition line counted as a caller, and all four
consumers are in `frame/`. Counted with `grep -v 'inline float'`.)*

## The one non-move: `kFrameDone` vs `kFrameExit`

The only substantive change in the split, and the non-obvious part of the frame loop —
the file split itself is mechanical. `frame/frame.hpp:24-25` defines two sentinels
replacing the loop's `continue`/`break`:

- **`kFrameDone`** — the path handled the frame; go round again (was `continue`).
- **`kFrameExit`** — leave the loop, run the shutdown epilogue, return 0.

The asymmetry this preserves is easy to lose and **was** lost in the original: a
`break` out of the loop ran the ImGui / device shutdown epilogue, but a `return` from
*inside* the loop did not — so a failed submit, or a written HUD shot, skipped cleanup.
Returning a **non-negative status** would have collapsed `kFrameExit` into
"keep going", because both are non-negative. The distinct sentinel is what keeps the
cleanup reachable. `frame/run.cpp:130-132` dispatches on it. Anything adding a new
early-return to either recording path must return `kFrameDone`, never a bare `0`.

## Frozen layout, measured

`36` `.cpp` files under `src/app` (`find src/app -name '*.cpp' | wc -l`; a TU count
that includes `chat_ui` — other denominators give 30). Largest `.cpp`:
**441** lines (`edit/live_edit.cpp`). Largest header: **539** (`app.hpp`).
`frame/run.cpp` is a **153-line** orchestrator and the only file that knows the
frame-loop order. Recorded with the command so the numbers are reproducible rather
than quoted.

## The trap the split laid: a renamed local that shadows a new member

The one bug the split's own verification could not see, and the most transferable
thing here. Promoting the loop local `shots` to the member `m_shots` by word-boundary
rename turned `std::vector<ShotSpec> shots = args.shots;` into **a local that shadows
the member**. It compiled clean, the body stayed byte-identical so the slice
verification correctly passed it, and `--shot` hung forever; fixed as an assignment
(`m_shots = args.shots;`, `frame/run_startup.cpp`). The signal that caught it was
**timing** — ~11 s became ~600 s. The principle is on
[[concepts/measurement-discipline]]: a verification method's blind spot is determined
by what it compares, so keep one signal from a different class.

## Verification, and what the split bought

Gates at completion: `--selftest` PASSED at 75.9 % coverage (identical to the
pristine-HEAD control); the hero render inside the control's noise floor; the three
headless hooks (`VF_TEST_EDIT`, `VF_TEST_STROKE`, `VF_TEST_UNDO`) emitting **identical
log lines** to the control; `--mode svo` still running. Note the control was built
specifically so the comparison could not be assumed — and the gate is
statistics-within-tolerance, not a hash, because the splat backend is **not
bit-reproducible** (an instrument that cannot fire is worse than no gate).

New `test-app` group: 34 cases / 192 assertions over the app's **pure leaf** logic —
gizmo screen maths (`ui/gizmo_math.*`), the command line (`cli/args.*`) and the
sidebar vocabulary (`ui/ui_primitives.*`, `ui/ui_types.hpp`) — all of it
**untestable before**, because those units shared a translation unit with `main()`.
It is a subset of `test-unit`, so the existing entry point still covers them.

**Three of those new gizmo cases failed on first run, and the expectations were wrong,
not the code** — see [[concepts/measurement-discipline]] for why that is the *good*
outcome for a brand-new gate.

## The split's worst trap: a rename that turns a local into a shadowed member

The mechanical move from a monolith to members is a **word-boundary rename**, and
that is exactly where the split can silently break behaviour while every existing
gate stays green.

Found here on 2026-09-26: promoting the frame loop's local `shots` to the App
member `m_shots` by rename turned

```cpp
std::vector<ShotSpec> shots = args.shots;   // local, initialised from args
```

into

```cpp
std::vector<ShotSpec> m_shots = args.shots; // *initialises the member*
```

That compiles with no warning, and it **shadows** the member: the declaration now
constructs `App::m_shots` from `args.shots` at construction, while the frame loop
keeps reading the still-empty `App::m_shots`, because the intended assignment line
was itself renamed into a redeclaration. `m_shots` therefore stayed empty forever.
Symptom: `--shot` **never terminated** — the capture is gated on
`if (shotMode && m_frameIdx == 3)` and `shotMode` is `!m_shots.empty()`, so the
app looped submitting frames (`VF_TRACE` showed `[fN] headless submitted`
indefinitely) and never wrote the PPM. No crash, no wrong pixels, no build error.
Fix: write it as a plain assignment in the body, not as a renamed declaration.

Three reasons this slipped through everything, and each is a lesson worth keeping:

- **Byte-exact verification cannot see it.** The function body was byte-identical
  before and after; only the declaration changed. A diff- or hash-based comparison
  of the *body* is the right tool for a pure move and the wrong tool for a
  promotion.
- **`--selftest` does not render a shot**, so it structurally cannot observe this
  bug. `test-visual` renders `--shot` and would have failed in about a minute. A
  gate that does not exercise the changed surface is not weak evidence — it is
  *no* evidence, and reporting it as one is the actual failure. See
  [[concepts/measurement-discipline]].
- **A member and a local can coexist in one scope**, so the compiler is not
  obliged to warn. Reach for `-Wshadow` when a refactor introduces this class of
  name, and prefer an explicit assignment over a renamed declaration whenever a
  local becomes a member.

If a headless run hangs or loops forever with no output, suspect an
always-empty gate variable before suspecting the renderer.
