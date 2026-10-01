---
title: Measurement discipline — confirm the instrument can produce the result
tags: [debugging, methodology, testing, tooling, review]
sourceRefs: [tools/fetch_textures.py, tools/check_texture.py, tests/visual_check.py, tests/live_edit_check.py, tests/visual_check.py, tools/record_demo.py, tools/test_inject_iso.py, docs/testing.md, AGENTS.md]
lastReviewed: 2026-09-26
---

# Measurement discipline

**A result is not evidence until you have confirmed the instrument can produce
it.** This page exists because the same failure recurred five separate times
in one session, each time producing a number that was confidently reported and
wrong. Every instance had the same shape: the measurement ran, produced a
plausible value, and the value was believed without a positive control.

## The eight instances (each one nearly filed as a finding)

- **Hand-decoding `assets/heightmap.png`** to recover terrain heights, which
  disagreed with what `--probe` reported from the real field. Two instruments,
  two answers, and the hand-decoded one was never validated.
- **An ffmpeg probe missing `-y`**, so a re-read silently returned the
  *previous* PNG. The same stale image was read six times and nearly reported
  as fresh data — the strongest single argument for making every capture
  overwrite by construction rather than by remembering a flag.
- **A `pgrep` wait-loop that matched its own command line**, so the "wait until
  the renderer exits" loop terminated on itself. Same family as the
  self-matching `pkill` in [[entities/hud-sidebar]]'s conventions: a process
  pattern that also matches the invoking shell destroys or fakes the
  measurement. Defeat it with a bracket (`build/[v]oxelforge`).
- **A "bright window" reading latched onto another session's render window.**
  In a shared-workspace repo with several sessions running, "a window exists
  and is bright" is not evidence about *your* process. Check the PID, not the
  title.
- **The shared build and display.** Other sessions relink `build/voxelforge` and
  take the GPU/display with no warning, so a result that looks impossible is
  usually **cross-session state** rather than a bug you just caused. Before
  believing a wild number, check `git status --short -- src/ shaders/` (someone
  may have rebuilt under you) and `./build/voxelforge --selftest`. This repo
  runs several concurrent sessions by design, so "the binary I am measuring" is
  itself an assumption that needs confirming.
- **A capture-target guard of the author's own** that blocked four runs —
  twice on benign window-manager windows, and twice on its own bugs, one of
  them a swallowed exception that reported a false obstruction. An
  instrumentation failure that reports confidently is worse than no
  instrumentation.
- **`clang-tidy … 2>/dev/null | grep -c warning:`** — clang-tidy writes its
  findings to **stderr**, so this reports a confident, wrong `0` and reads as a
  clean bill of health. It produced a real contradiction during the clang LSP
  setup: the same files measured `0` this way while the language server, over an
  identical check set, reported 32 and 1161. Same family as the ffmpeg `-y`
  instance above — the measurement never touched the data it claimed to read.
- **`clangd --check`'s "All checks completed, N errors"** is not a diagnostic
  count. It counts log lines emitted at ERROR level, and the `ExtractFunction`
  refactoring probes log at ERROR whenever they do not apply. A file with zero
  real diagnostics reported `19 errors`. A summary field that counts the wrong
  thing is a measurement bug wearing a diagnostic's clothes — check what the
  number is a count *of*.

## Consequence: silence is not a clean bill of health

The generalisation, and the rule to carry into any review:

> **An absent signal is only evidence if you have shown the probe can fire.**

Concretely, in this repo:

- A default run of a `VF_TRACE`-gated probe printing **zero lines** cannot
  distinguish *the guarded path is healthy* from *the code never reached the
  path it guards*. The firing direction has to be demonstrated positively
  (force the condition, see the line) before a quiet run means anything. A
  probe that has only ever been seen quiet is untested, not passing.
- A **1 Hz sampling series** is the right instrument for seconds-scale
  transitions and is **below Nyquist for one-frame events** — e.g.
  `m_taaFirstFrame` forces blend `0.0` for exactly one frame. A null from it
  is undersampling, not evidence of absence.
- A green `ctest` group only covers the paths that group exercises. A
  behaviour with no headless seam (see the click-vs-drag gate in
  [[entities/live-edit-brush]], which reads `glfwGetMouseButton` directly) is
  **manual-verify only**, and a green suite is not evidence about it.

## A NEW log field is a NEW instrument — and its first reading is the least trustworthy

The sharpened form of "confirm the instrument can produce it", earned the hard
way: **the first reading from a newly added field deserves no more trust than
any other reading.** A field that has never been calibrated against anything
produces the *least* reliable data in the system — and gets read with the
*most* confidence, because it is the newest and it appears to answer exactly the
question being asked.

Observed exactly this way: a `pick object` / `pick terrain` field was added to
the stamp log, read `terrain` off a cell that was in fact a cabin cell, and
believed **over a world that had already been measured and mapped**. The
eleven-hour-old rule ("a null from a broken instrument is not evidence of
absence") was cited by the person making the error and did not prevent it,
because the error was not a null — it was a confident, plausible, *wrong* value
from an uncalibrated source. The reader was broken, not the world: the headless
hook built its pick by hand and never set the field, so every headless run
reported the same class regardless of what it actually touched.

**Calibration needs a case where the new field and an independent source must
disagree.** That is what exposed the hook here: two cells that were obviously
wood logging `terrain`. No amount of re-reading the code that produced the
field would have shown it, because the code was self-consistent.

**Corollary — a broken instrument does not always spoil the number.** In that
same episode the headline figures (a run-split count and a whole-frame pixel
diff) were produced by paths that never touched the broken field, so they were
never in question; only the *label* attached to them was. Reflexively
distrusting a number because a nearby instrument is broken throws away good
data. Say precisely which measurement the broken instrument was *in*.

## "Never taken" is worse than "behind an unset guard" — and it looks fine in the source

An upgrade to the rule above, earned by a hook that **fired zero times** while
reading perfectly correctly in review. The forced-failure probe was first placed
beside `vkQueuePresentKHR`. It never ran — because **a headless render never
acquires or presents the swapchain at all**: it submits its command buffer and
reads back the *offscreen* image (`headless submitted`). So:

> **"Has this run?" has to mean "can it run in the configuration the tests
> use?"** — a stronger question than whether a guard is set.

This is the worst failure shape in this file, because every static signal lies.
The lines are **covered** (they compile, they are in the binary, coverage counts
them) while the code **never executes**. No coverage number shows it. Reviewing
the source shows nothing wrong. The only thing that finds it is running it — and
running it requires *knowing* it is unreachable, which is the thing you cannot
see. Fix pattern worth reusing: when a diagnostic is only reachable from a
windowed path, give it a synthetic driver **and call it from the headless frame
body**, not only at the real call site.

Corollary for test design: a check's own docstring can preserve the *pre-fix*
reasoning and quietly send the next person to undo the fix. Prefer comments that
state the current invariant ("reachable because the headless body calls it") over
ones that narrate the bug hunt.

## Assert every string surgery

Cheapest lesson of the review, and it cost a full build. A scripted
search-and-replace patch **silently did nothing** because the anchor had the
wrong leading whitespace, and nothing complained. The compiler caught it — but
only by accident, and a text edit to a *docstring* or a log line has no compiler
at all.

> After any programmatic string edit, assert the replacement actually happened
> (count occurrences before and after). A no-op edit is the most expensive kind
> of silent failure, because it looks exactly like success.

## A comment that records *why* is a claim with an expiry date

The same failure with a different trigger, and the pair belongs together. A
comment saying "X is unreachable from every test, so the hook lives here" was true
when written; a later change made it false; the stale sentence then read as a
**live constraint** and would have invited the next person to move the hook back
and re-break it. Nobody flagged it, because **reading a comment feels like
reading code.**

The unifying rule, and the reason both cost real time:

> **The expensive silent failures are the ones with no compiler behind them — a
> no-op edit and a stale comment are both silent, and both look exactly like
> success. The defence is the same for both: _assert it_, or _date it_.**

Either state the invariant in the present tense ("reachable **because** the
headless body calls it", not "unreachable from the real loop, as we found"), or
mark the comment as history ("the first version shipped this way, firing zero
times"). A justification in the present tense will eventually be a lie, and it
will be believed, because prose has no type checker.

A related trap in the same family: a justification that was true can become
*irrelevant* while a weaker one takes over. A check kept using `--smoke` for a
reason that had quietly changed from **reachability** (both headless modes work)
to **statistical power** (3 frames cannot distinguish a working latch from a
broken one). Merging the two reasons into one sentence is how the stale version
survived.

## A verification method's blind spot is determined by what it compares

The sharpest lesson of the `src/app` split, and it is a **code** trap rather than a
measurement one — so no amount of framing would have surfaced it.

Promoting the frame loop's local `shots` to the member `m_shots` by word-boundary
rename turned

```cpp
std::vector<ShotSpec> shots = args.shots;   // into: a local that SHADOWS the member
```

It **compiled clean**, the function body stayed **byte-identical** so the slice
verification correctly passed it — and `--shot` hung forever. Fixed as an assignment
(`m_shots = args.shots;`, `frame/run_startup.cpp`).

Two things to keep:

- **Body-diffing cannot see declaration-level changes.** The method's great strength
  here — proving no *logic* moved — is precisely what made the bug invisible, because
  the bug lived in a declaration. The artefact it protects and the class of bug it
  cannot see are the same axis. "Byte-identical body" is the strongest evidence of a
  *pure move* and is **not** evidence that a move was pure.
- **So keep at least one signal from a different class.** Here it was wall-clock: the
  render went from ~11 s to ~600 s. No diff, no hash and no byte-comparison would have
  flagged it; a *timing* regression did. Whenever a verification passes, the useful
  question is what that method is structurally incapable of seeing — and then whether
  any surviving signal covers that class.

## A gate result is evidence about the gate, not about the change

The same session reported sending three peers *"builds clean, gates pass"* on the
strength of a green compile plus a `--selftest` — on a change whose entire surface was
**shot capture**, which `--selftest` never exercises. The sentence was true of the
signals that were run and false as a claim about the change, and nothing in the
artefacts would let a reviewer detect it by re-running anything.

Corollary: *"2/2 green"* is a statement about a named group on a named revision. It is
not a statement about the change unless the group provably covers the change's surface.
Check the coverage argument before repeating the result — and if the change's surface
is a stage the gate does not reach, say that instead of reporting the green.

## A gate that has never failed is not yet known to fire

A brand-new `test-app` group landed 34 cases / 192 assertions over the app's pure leaf
logic (gizmo screen maths, `parseArgs`, sidebar vocabulary) — all of it **untestable
before** the `src/app` split, because those units shared a translation unit with
`main()`. **Three of the new gizmo cases failed on first run, and the expectations
were wrong, not the code:**

- assumed forward is `-Z` at yaw 0 — it is **`+X`** (forward is `-Z` at yaw −90°);
- assumed a point at a ring's **centre** has distance 0 — it is **one radius** away;
- assumed `+X` along the pitch ring is unambiguous — the pitch ring is **nearer** there
  in normalised terms.

Each was **measured with a throwaway binary before a line of the test was changed.**
That is the correct outcome for a new gate, and it is worth stating plainly because the
instinct treats a first-run red as a problem with the code: a gate that has never
failed has not yet demonstrated it can distinguish right from wrong, and three
same-day failures are the cheapest possible proof that it can.

The corollary for reviewing new tests: **ask what the gate's failure mode is before
asking whether it passes.** A green brand-new test is weak evidence; a red one that
turns out to be a wrong *expectation* is strong evidence, because it means the author
checked the code's answer instead of asserting their own.

## A green signal that does not cover the change is not weak evidence — it is none

The preceding section is about a gate that has never *fired*. This is the
stronger and more common form of the mistake: reporting a verification that
**passed**, where the thing that passed does not exercise what changed.

Seen on 2026-09-26, twice in one day, by a session that was otherwise careful.
During the `src/app` split, stages 2/3 were reported as "builds clean, gates
pass" on the strength of a green compile plus `--selftest`. But the change was
the frame loop's shot-capture path, and **`--selftest` never renders a shot** —
it structurally cannot observe that path. `test-visual` *does* render `--shot`
and would have failed in about a minute. It did fail, shortly after, for an
unrelated reason that a byte-exact body comparison had correctly passed over: a
word-boundary rename had turned the frame loop's local `shots` declaration into
`std::vector<ShotSpec> m_shots = args.shots;`, which **shadows** the member
instead of assigning it. `m_shots` stayed empty, so `shotMode` (defined as
`!m_shots.empty()`) was never true and `--shot` looped forever without writing a
PPM. Clean compile, no crash, no wrong pixels, byte-identical body — see
[[entities/app-subsystems]] for the full mechanism.

The rule, stated so it is usable next time:

- Before reporting a gate as verification, **name the behaviour it exercises and
  check that the change touched it.** A gate that does not cover the change is
  not a weak signal, it is an absent one.
- `--selftest` is a sky probe plus coverage. It is **not** a shot renderer and
  **not** a behavioural gate. Reaching for it by default because it is the
  cheapest is how a claim ends up resting on the wrong instrument.
- Cheap-and-always-runnable is a virtue for *smoke*, not for *verification*.
  When the two conflict, the group that renders the thing wins.
- A byte-exact comparison of a moved function's **body** is the correct tool for
  a pure move and provably blind to a changed **declaration**. Know which
  question your instrument can answer.

The general shape is the same as everywhere else in this file: the failure is
never "the measurement was imprecise", it is "the measurement was about
something else, and nobody checked".

## A red with no change attached points at the environment, not the code

Several sessions share this working tree, the GPU and the display, so a result can
change without anything being edited. The rule that generalises is **not** "a number
that looks impossible is probably someone else's" — that asks the reader to judge
*impossibility*, which is subjective and gets argued about. The mechanical version:

> **If a gate goes red and nothing has changed since the last known-good, suspect the
> environment before the code. Re-run it alone.**

Observed exactly: `test-live-edit` failed in ctest with `live_edit_check` named, on a
change that had not touched it. Cause was four test instances contending for one GPU —
`test-visual` and the store/world batch launched concurrently. The script passed
standalone and 3/3 on a re-run with nothing else running. The discriminator was not the
failure's severity but its **provenance**: a red with no diff attached. The correct
response was to re-run it alone — *not* to trust the red, and *not* to go hunting a
regression in code nobody had changed.

**Run groups sequentially whenever the GPU is shared.** Concurrency makes a render or
GPU-bound gate red for reasons entirely orthogonal to the diff, and the failure names a
test that is innocent.

## Both failure modes on one change in one day

The sharpest statement of why this page exists, and it came free from the same
session. On a single refactor, in one day:

- a gate went **red with nothing changed** — the *result* was wrong (GPU contention);
- a gate reported a **difference on an unchanged binary** — the *instrument* was wrong
  (a `sha256` render gate on a splat backend that is not bit-reproducible).

Result-wrong and instrument-wrong, same change, same day. Neither is visible by
re-running the same gate harder, and each would have been filed as a code defect by a
reviewer who trusted the artefact. **When both the result and the instrument are
suspect, suspect the instrument first** — it is the only one of the two that is
usually wrong for reasons outside the change.

Other shared-state cases, all measured:

- **The test suite writes into `assets/`.** `tests/test_world.cpp` and
  `tests/test_authoring.cpp` set `dir = VOXELFORGE_ASSET_DIR` and write
  `dir + "/world_all.json"`, so `test-world` / `test-unit` / `test-surfel` **rewrite a
  tracked file in the versioned asset directory**. A concurrent reference-render
  verification can therefore be perturbed by another session's test run, and "treat
  `assets/**` as read-only" is a request rather than an enforced property. The write is
  also a bare `std::ofstream` with **no temp+rename**, so an interrupted run leaves a
  truncated manifest and every content test fails on it — the overlay writer in this
  codebase already uses temp+rename, so the pattern to copy is in-tree.
- **Four concurrent test instances on one GPU produced a red that was contention, not
  a regression.** A `test-live-edit` failure traced to four instances competing for the
  device; re-run alone it was 3/3. The tell is available in advance: run groups
  **sequentially** when the GPU is shared, and treat a red that appears only under
  concurrency as a contention hypothesis until proven otherwise.
- **Three tracked assets were deleted outright** (`world.json`, `heightmap.png`,
  `landscape.vxw`) while another session was mid-gate, breaking it. Those three are
  *exactly* `heightmap_gen`'s output set, which is what made the suspect set small —
  and the cause turned out to be the reporter's own `ninja -C build -t clean`, since
  the authored scene is **registered as build outputs**. See
  [[concepts/authored-assets-are-build-outputs]]. The general lesson is the one worth
  keeping: when a versioned input vanishes, the question is not "who deleted it" but
  "what in this build system believes it owns that file" — and an exact count of
  surviving registered outputs is what confirmed the mechanism instead of merely
  suggesting it.

Rule of thumb: before blaming a surprising number on the code, ask **who else could
have written the thing being measured**, and check `git status --porcelain` plus the
mtime of the input. And when a session reports "I did not do this", make the answer
*checkable* — an exact list of files written, and the exact commands run — rather than
asking anyone to take it on trust.

## Opposite failures, one cause: the mistake is almost always in the reading

Two mistakes in one night, in opposite directions, with the same root:

- A **new log field read a plausible CONSTANT** — `terrain` on every cell,
  because the hook that fed it never set it. Believed over a world already
  mapped.
- A **correct tool read as BROKEN** — `vf_slice --axis z` appeared to render a
  blank plane. It does not. The output had been piped through `sed -n '4,26p'`
  to "trim" it, and those rows are simply **above** the terrain: the grid prints
  **row 0 as the top of the span with rows descending** (50 of 102 rows have
  content, including rock and soil). The tool **says so in its own header line**
  (`(cols: … asc, rows: … desc)`, `tools/scene_slice.cpp:77`) and again in the
  loop comment at `:85`. The instrument told you; the reading missed it.

Re-reading the producing code found **neither** fault, because in both cases the
code was self-consistent and correct. **A plausible constant and a plausible
blank are not evidence.** They are the two shapes a reading takes when nobody has
checked whether the instrument can produce the thing being looked for.

So the rule, in the form that would actually have caught both:

> Before believing **or** condemning a reading: look at the **whole** output, not
> a convenient slice of it, and cross-check it against a source that **cannot
> share the failure mode**.

The second half is what makes it a check rather than a habit. The world has a
second way to ask what material is at a cell (`--probe`, and the glyph legend
in the slice header), so a disagreement is available for the price of one extra
command. An instrument cross-checked against something that shares its code, or
shares its author's assumptions, is not a cross-check.

Corollary for pipelines: a `sed`/`head`/`tail` window over a tool's output is a
**new instrument** with its own failure modes, and it is added casually. Prefer
reading the tool's full output, or its documented conventions, before shaping
it.

## Granularity of claim: switch vs cause

A single keypress can support a statement about a **switch**, never about a
**cause**. "TAA-off held across N launches" is supportable; "TAA is fixed" is
not, and the difference is invisible until someone builds on it. The same
discipline applies to *mitigation vs fix*: a NaN guard that converts a
visible artefact into a silent one is a **mitigation** — it makes the
symptom harder to see, not the bug less real, and it must never be reported as
a fix.

Corollary for intermittent states: a dark window that is bright on one launch
and never bright on another, same binary and same GPU, **has no cause yet**.
Any page implying a settled explanation is overstating it. Narrowing to a
single remaining suspect by elimination is a *hypothesis with a one-keypress
test*, not a finding — and the test has to be run.

## Never freeze a content or asset name in a test

A separate, closely-related instance: read the subject from the manifest at
runtime, keyed on a **stable contract** (a `role`, a schema field), never a
filename. One frozen name frozen twice in one script became N confusing reds
that looked exactly like a renderer regression while every shot-acceptance
check passed. Derive the path from the same env the app was launched with. See
[[concepts/focused-test-groups]] for how the groups gate bodies.

## Before filing any number

1. What would this instrument report if the effect were **absent**? If the
   answer is the same as the healthy case, the instrument is useless here.
2. Can I produce the effect **on demand** to prove the probe fires?
3. Is the sampling rate fast enough for the timescale of the effect?
4. Is this process/window/asset **mine**, or one I merely found?
5. Am I reporting a switch, a mitigation, or a cause — and does the evidence
   match that word?

Cross-refs: [[concepts/focused-test-groups]] (what a green group actually
covers), [[entities/live-edit-brush]] (a manual-verify-only behaviour),
[[concepts/x11-input-injection]] (driving the real window, where most of these
instances arose), [[concepts/load-time-field-build]] (a measured cost that
was worth trusting *because* its phases were measured separately).
