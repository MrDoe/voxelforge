---
title: Measurement discipline — confirm the instrument can produce the result
tags: [debugging, methodology, testing, tooling, review]
sourceRefs: [tools/fetch_textures.py, tools/check_texture.py, tests/visual_check.py, tests/live_edit_check.py, tests/visual_check.py, tools/record_demo.py, tools/test_inject_iso.py, docs/testing.md, AGENTS.md]
lastReviewed: 2026-10-08
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

## The most expensive defects here are not wrong numbers — they are right numbers in the wrong place, unlabelled

Three instances, one category, and it is worth naming because all three looked
like missing instrumentation and none of them were.

| instance | the value was | what was missing |
|---|---|---|
| `night_check.py` day mean | **computed and printed on every run** (`:162-164`, `mean %7.2f -> %7.2f`) | a label, so nobody read the left figure as an absolute anchor. The scale-blindness of the ratio gate looked like a missing instrument; the instrument was in the output all along. |
| `visual_check`'s `is_sky` | a correct classifier term | the `b > 120` daylight constant, which made it classify a valid night sky as *object* — and made coverage fail as too *little* sky |
| `skyVisibilitySPlat` | a correct one-ray occlusion test | the decision not to consult `heightfield`; `heightAt` says "solid" below the surface, so every interior read as underground |

The shared shape: a correct quantity is available, and its meaning is not
recoverable from where it sits. A number with no label is not evidence, however
true it is.

**Two of the three had a comment already explaining the correct behaviour** —
inside the code, adjacent to the defect. That is the part worth internalising:
reading a function's own comments is not politeness, it is often the only place
the intent is recorded at all. Twice today a page was written from comments that
had been in the source the whole time, and twice the first reading of them was
wrong in a direction the comment would have prevented.

Corollary for review: when something looks unmeasured, **check whether it is
measured and unlabelled before proposing a new measurement.** A proposal to add
an instrument should be preceded by a check for an existing one — otherwise the
new instrument arrives, is trustworthy, and quietly duplicates a value nobody is
reading.

## Rank the claim by the evidence you hold, and let that choose where it lives

### A log entry is a pointer with provenance, not a restatement of the page

Whatever lives on a content page does **not** belong again in `log.md`. An entry
carries what the page cannot: what changed, the numbers **with their
configuration**, what is still open, and who owns it. If an entry can be deleted
without losing a single mechanism, it was duplicating a page.

The failure mode is specific and it is invisible in review, because a long entry
*looks* thorough: the knowledge layer grows two copies of each finding, they
drift apart, and a reader who finds the log copy has no way to tell it is
second-hand. `log.md` reached 298 KB / 4660 lines over 95 entries — median 22
lines, so a handful of narrative entries were carrying the bulk. Prose about who
said what to whom is chat transcript, and a transcript in a knowledge layer is
history pretending to be reference.

**Corollary for provenance:** a correction belongs in the log even when the page
is what gets fixed, because "this was wrong, here is why" is the fact a reader
cannot reconstruct from the corrected page alone.

Earned 2026-10-08 by filing a finding that was wrong. The sequence is worth
keeping because each step was individually reasonable.

Two backends folded apparently different values into the same helper, which
looked like a divergence — so it was filed as a lead in a content page, tagged
**"structural, not measured"**, with a proposed A/B. It was retracted the same
day: the helper opened with its own bitmask guard that made the difference
unreachable in exactly the state the test proposed. No GPU time was spent and no
code changed, which is the only reason the outcome was cheap.

The hedge is the part that made it expensive. It was filed as intellectual
caution, but its effect was to make a wrong claim **survivable** — a lead in a
page reads to the next session as settled-but-unexplored, and that is the state
that costs someone an edit.

> **"Structural, not measured" is not a third state. It is state (a) wearing a
> disclaimer.**

### The rule

| evidence you hold | what you may write | where it goes |
|---|---|---|
| consistent with source, read end to end | assert it | a content page |
| **contradicted** by source | delete it, keep the reasoning | `log.md`, as a retraction |
| neither — you cannot settle it | **open question** | `log.md`, or a message to whoever holds the instrument. **Never a content page.** |

The load-bearing part is the last row, and it is testable on your own tree:

```sh
grep -rnE "structural, not measured|unmeasured|unvalidated|untested|not compared" \
     concepts/ entities/
```

A content page asserting an unmeasured divergence is a defect **regardless of
the heading's tag**. Swept on 2026-10-08: the hits were all *disclosure*
(tables saying "unmeasured", "George-reported, not measured by the wiki
session") or *terminology* (`sh = 1` meaning unmeasured rather than lit), plus
one borderline — `entities/live-edit-brush.md` labelling a proposed test
"**unvalidated** — a proposed test, not a working one", which correctly
discloses the test's status but sits in a page about a mechanism a reader may
assume works. Refusing to assert is not the same as asserting while hedging.

### A call-site comparison is not a behaviour comparison

The retracted claim was correct about both call sites and wrong about the
system. Small pure helpers with a bitmask guard are **where the decision
lives**; call sites are where the plumbing lives. So the actionable heuristic is
not "read the callee first" in general — it is:

> **Pure leaf helper, no I/O, short body → read it whole.** It fits on a screen
> and it is the thing you are reasoning about.

That is recognisable in a second, unlike an instruction to be more careful. The
guard was three lines from the call site and had already been read earlier in
the same session; the evidence was present and unconsulted, because the claim
was already formed and the hedge made it feel *unfinished* rather than *wrong*.

Same shape as two neighbours in one exchange: a `${f%.*}` probe that passed while
wrong, and a catalogue sweep clean on one direction and eyeballed on the other.
**Three results that were clean at the layer inspected and uninformative at the
layer that decides.** When a result is clean, ask which layer you actually
tested — and see [[concepts/measurement-provenance]] for the related trap of a
number whose provenance does not survive being quoted.

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

## A guard defined relative to the feature it guards disappears with the feature

Ask this of **any deletion**, not just code: *which assertions, gates and checks
were expressed in terms of the thing being removed?*

Worked example (micro-surfel removal, ordered 2026-10-08): the live-edit check
asserted **"the patched run grows substantially with `VF_MICRO` on vs off"** —
so it *compared the feature against its absence*. Deleting the feature deletes
that assertion, because the assertion is not about the property being protected;
it is about the feature existing. The property that actually matters is *"a
patched chunk's regenerated run still covers the cells the stamp touched"*, and
nothing in the old assertion names it.

> **A test whose subject is the feature is not a guard on the feature's
> behaviour.** The replacement must be phrased against the surviving system, or
> the next person to touch that code inherits a guard with a hole shaped exactly
> like the deletion — and it is invisible, because the guard and the gap arrive
> in the same commit.

The cheap review form: after a deletion, grep the surviving checks for the
deleted identifier. Anything still naming it was either rewritten correctly or is
about to be deleted quietly. And prefer replacements that name **no** part of the
removed system — "non-empty patched run, count differs from pre-stamp, base and
bridges regenerate" survives a second deletion that the original never could.

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

## Confirm the instrument's IDENTITY, not its health

Every section above is about a check returning the *wrong answer*. This one is
narrower and sneakier: the check returns a **clean** answer while the thing
under test is broken.

A `.glsl`/`.comp` edit does **not** change `build/voxelforge`. Measured
2026-10-07 across two shader windows: the binary's md5 was **identical**
(`07306f1c…`) on both sides, because shaders compile to `build/shaders/*.spv`
and load at runtime — they are not linked into the executable. So during a
shader edit:

- `md5sum build/voxelforge` — clean
- its mtime — old
- `strings` — finds nothing new
- `ninja` — may report "no work to do"

…and yet the **render is wrong**. The only reliable tell is
`build/shaders/*.spv` (md5 or mtime). A stale `.spv` produces *plausible* pixels
from the previous shader, so nothing downstream flags it and every number taken
during the window looks like data.

Two rules:

1. **Never render, measure, or screenshot inside another session's announced
   shader window.** If you might have, re-render before trusting anything.
2. **On a shader window close, require proof on the artefacts that actually
   moved:** `md5sum -c` on the `.spv`, a diffstat matching the expected
   baseline line counts, and a grep of the **source** file for the reverted
   constant — never of the binary.

Robin caught this by noticing the misleading symptom was the *frame*, not the
`.spv`, and that no check available on the executable could explain it. The
generalisation: **verify the identity of the instrument, not its liveness.**

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

## Structural lint: checks that need no judgement

Three axes, and the distinction that matters is what each one compares against:

| axis | question | compares against | needs a human? |
|---|---|---|---|
| presence vs uniqueness | does every page appear in the catalogue; every link resolve? | the catalogue | no |
| presence vs settledness | does this claim still describe the thing it was written about? | the code as it is now | **yes** |
| structure vs neighbour | is this line still attached to what it belongs to? | **its own neighbour** | no |

The third is the least skippable because it has no opinion about what the text
should say, so it cannot be fooled by prose that reads well. Every
content-judgement check on this page was fooled by exactly that.

### Orphaned table rows

A `|` line **not** preceded by a table row is an orphan **unless it is itself a
header** — i.e. unless a separator row (`|---|`) follows. Blank lines do not
break a table; intervening prose does.

### `edit` anchored on a heading silently deletes it

`edit` matches on text and has no notion that the string being replaced **is** a
heading, so "insert before X" becomes "delete X" whenever X occurs once. Nothing
complains: the file stays valid markdown, links resolve, prose reads.

> **Re-emit the anchor in the replacement, then verify it survived.**

Instrument: `git diff -U0 <file> | grep -E '^[+-]## '` — a deleted heading shows
as `-## …` with no `+##` counterpart. **Two documented limits:**

1. **It only sees headings present in `HEAD`,** so a heading added *and* deleted
   within one uncommitted session is invisible. For uncommitted work, diff
   against the file as it was before the edit.
2. **It cannot distinguish a deletion from a rename.** A correct rename appears
   as an unmatched `-## old` / `+## new` pair and is indistinguishable from a
   deletion plus an addition. Confirmed in practice: `voxel-object-authoring.md`
   reported a lost `## Why SDF-in-code (no mesh import)` when the heading had in
   fact been **correctly renamed** to `## Why SDF-in-code is the primary path` —
   the old title was stale, because STL/OBJ import does exist.

> **Resolve every finding by reading the section, not by trusting the diff.**
> A rename is a *fix* and a deletion is a *defect*, and they look the same in
> the instrument. The check narrows the search; it does not decide it.

### The non-unique anchor: `edit` matches somewhere you did not look

The rule above assumes the anchor is **unique**, so replacing it removes
something. When it is not unique, `edit` matches the first occurrence it finds
and **reports success** — you wrote new text into a *different* table than the
one you were reading.

> **The failure is not only "deletes the heading"; it is "silently matched
> somewhere else".** And the non-unique anchor is the one that feels safe, since
> it is visibly repeated and therefore obviously targeted.

This is strictly worse than the unique-anchor case: nothing is missing, the edit
succeeded, and the damage is only visible by comparing against what you meant to
write. **Verify the match, not the write** — `grep -c` the anchor first; a count
above 1 means re-anchor on more surrounding context before editing.


### A heading split from its body

Anchoring on a heading and inserting immediately below it separates the heading
from its body. Both survive, the file stays valid, **and no presence check sees
it** — the only damage is that content no longer sits under its own title.

> **Anchor on a heading only if you re-emit everything below it.**

`grep -A3 '^## ' <file>` — a heading whose following lines are blank, fenced, or
another heading has been split.

### Why the order axis is the one worth adding

Presence checks find what is **missing**. This finds damage where **nothing is
missing** and the content is merely in the wrong place. That class is strictly
worse: deletion shows up in a diff, reordering shows up nowhere.

Working example (2026-10-08): moving a note below a table left the table's last
two rows behind, duplicating them under a blockquote. A row-count check against
a remembered structure caught it; every content check read the file as fine.

## A test that chooses its own subject cannot detect that the subject set is truncated

The closed loop above is a test that **re-implements** the expression under test.
This is the stronger form, and it survives an independent reference: **a test
that selects its own subject cannot detect that the subject set is a fraction of
what it should be.**

In the irradiance volume (2026-10-08), the six tests **passed with the bug
present** — 6/6, 153 assertions. The tests place a synthetic emitter wherever
`findOpenAirAirCell` lands. The bake's slice loop used the slice *count* as a z
*stride*, so it covered a quarter of the volume — and the quarter it covered
happened to contain the spot the test had chosen.

Note what this defeats: **even a hand-computed expected value would have passed**,
provided it was computed over the same truncated band. That is not a loop between
two implementations. It is a loop between a test and *the data it picked*, so
adding an external reference does not fix it — the reference has to be an
**enumeration** of the subject set, not a value computed from it.

> **The fix is a census, not an oracle:** assert *what the instrument visited*,
> not only what it concluded. "N subjects in range, 0 visited" is the signal; a
> green pass on the same run is not.

Instance fix **verified through `ninja` 2026-10-08** — see `log.md`. Green
configuration, reported by Vega: `vf_core` links (`ninja vf_tests`),
irradiance **7/7 cases, 180/180 assertions** via the ninja-built binary
(the seventh case pins the fixed subject, so the suite that passed with the
bug present both directions can no longer do so), `test-world` group 2/2 via
`ninja` (110.66 s), `build/voxelforge` md5 untouched throughout.

> **Label: current-tree until commit.** Vega's 4 files are uncommitted, so this
> green belongs to the dirty tree, not to a hash. The historical 6/6-153 figure
> above stays as the record of the bug-present run; the 7/7-180 figure is the
> fixed run that supersedes it.

## When a measurement reports "no effect", check that it enumerated its subject set first

Two instruments, one codebase, one light set, disagreed: **40 candidate pairs in
range, 0 visited.** The disagreement *was* the signal. Both hypotheses — the
author's and the shading session's — reasoned about **visibility and resolution**,
i.e. about the physics, while the actual defect was in the **iteration bounds**.

> **A clean zero is a claim about the subject. Before accepting it, confirm the
> instrument looked at the subject.** An instrument that reported a confident
> zero over a quarter of the world was not measuring a null result; it was
> measuring its own coverage, and reporting it in the subject's voice.

This is why "no effect" needs a stronger precondition than "effect" does. A
*present* effect is self-evidencing — if you can see it, the instrument reached
it. A *missing* effect is indistinguishable between the subject being absent and
the instrument never going there, and only a coverage claim separates them.

**Corollary:** disagreement between two instruments is evidence, not noise. When
two instruments on the same codebase report differently, the most productive
question is not which is right — it is what each one looked at.

## Purpose-built instruments keep answering a neighbouring question

Three times in one session, an instrument built to answer a specific question
answered a **different** one: a census built to test one hypothesis invalidated
**both** standing hypotheses. That is a pattern, not a coincidence.

> **Prefer instruments that report what they enumerated over instruments that
> report only what they concluded.** A census is reusable across every question
> about the same subject set; a verdict is disposable.

The practical form: an instrument that emits its own coverage — how many
candidates were in range, how many were visited, what fraction of the domain it
covered — is debuggable on a day when its answer is wrong, and a null result from
one is distinguishable from a null subject.

## A diagnostic that is byte-identical for two different causes

The irradiance bake logs `14 seen / 0 used / 0 cells lit`. That string is
**byte-identical** to the `VF_NO_IRR_VOLUME` fallback path, which returns its
stats immediately after emit collection. So the line cannot distinguish
*the volume loaded and is empty* from *the volume never loaded*.

> **A diagnostic shared by two causes carries no information about either.** It
> is not a wrong value — it is a correct value that cannot answer the question it
> is being asked. Any branch that would change the reported number must get its
> own token; a sentinel that means "empty" must be spelled differently from one
> that means "absent".

This is the sharpest form of *correct value, present, unread*: everything is
correct and nothing is legible. It is worse than a missing log line, because a
missing line invites a fix while an ambiguous one invites a conclusion. Filed on
[[concepts/enclosed-space-lighting]], where the descriptor is live and reading
zero.

## The author of a lesson reproduces it while writing it down

Wrote a page arguing that a coordinate formula must not be re-derived in two
places — then re-derived `- WORLD * 0.5f` inline at exactly the line the page's
own comment forbids, and attributed the finding to the reviewer who caught it.

Two independent facts, and both matter: the lesson did not transfer to the
act of writing the page about it, and **attributing the catch to a reviewer kept
the page credible while the error was still in it.** A page whose author can be
wrong in the page is not a reason to skip it — it is a reason to mark it
unverified until someone else has checked the references.

> **Do not treat a page's own line references as sound because the page is
> careful.** Careful is a property of a session, not a page.

## A test derived from the implementation cannot falsify the implementation

Found 2026-10-08 in the irradiance-volume bake (George's plumbing + Vega's
volume). `tests/test_world.cpp`'s `irrCellCentre()` copied the bake's **own**
uncentred cell formula (`p = (x + 0.5) * kCell`), so the bake and its test agreed
with each other while the **shader** — the only consumer reading the volume in
the real origin-centred world frame — sampled it **51.2 m / 32 cells** away on
every axis. **All 232 assertions passed.**

### The assertion count is not evidence

232 green assertions are not stronger evidence than 5. They are **the same
single check, counted 232 times**, and they would have reported the wrong frame
just as happily. Any metric derived from agreement *inside* the pair under test is
constant with respect to the thing you are trying to detect — so quoting it as a
confidence signal is not conservatism, it is arithmetic.

> **The number that can move is: how many inputs came from outside the pair.**

### Two frames, not a sign error

**The producer bakes in `0..WORLD`. The consumer reads in `-WORLD/2..+WORLD/2`.**
The bake used `(i + 0.5) * kCell`; the shader sampled `irrVolumeUVW =
p / WORLD + 0.5`. **Both sides are monotonic** — CPU `p` increases with `x`, GPU
`p` increases with `WORLD`-normalised input — so no axis can be flipped and the
only possible discrepancy is a constant **translation** of `WORLD / 2 = 51.2 m
= 32 cells` on every axis.

That is the whole mechanism, and both symptoms fall out of it:

- a room at world `y ≈ 1.5` reads the CPU's cell at `y ≈ 52.7` — above every
  surface — hence **sky wash**;
- the lamp's peak, stored at CPU `y ≈ lamp y`, renders at `y ≈ lamp y − 51.2` —
  hence **no lamp light**.

Two opposite-looking symptoms, one translation. The fix is **subtract
`WORLD * 0.5`**, not flip an index — which is why the wording matters. This page
first said "inverted / mirrored", inferred from the *shape* of two ranges rather
than from the mechanism. That was wrong in a way worse than silence: "mirror"
sends a reader looking for a sign error that does not exist, and it invited a
wrong fix that would have been made confidently. **A wrong mechanism reads as a
wrong symptom, which a careful reader discards — so a wrong mechanism is
worse than no mechanism at all.**

### The file disagreed with itself — the cheapest signal, unused

The same file's `heightAt()` / `objDistAt()` used `wx / WORLD + 0.5`, i.e. the
**centred** convention. So the file already contained the contradiction, and
diffing it against itself would have caught this with **no external authority at
all**. Most investigations never look for that, because they are looking for the
thing they just wrote.

This sharpens the usual escape route. It is not strictly "find a source outside
the pair" — it is:

- **look for a second convention already present in the file.** A file that
  disagrees with itself is cheaper to detect than a bug that is wrong
  everywhere.
- **read it from the consumer.** The only thing that broke the loop was George's
  plumbing-side read of `irrVolumeUVW` — a *consumer* of the output, outside the
  pair, reading it in the frame the consumer actually uses. Every bake has at
  least one consumer that knows the real origin, and the consumer is where the
  truth lives. (A reviewer diffing the CPU port against the GPU code caught the
  same thing: two files written from the same understanding, which therefore
  *should* have matched and didn't.)

### The fix that makes it non-reversible (Vega, 2026-10-08)

Advice a future session can decline is worth less than a constraint it cannot
talk itself out of. Vega's fix is the second kind:

> **The test keeps its own independent `indexOf` on purpose; collapsing the two
> would re-create the closed loop that hid the frame bug.**

Two index functions that **must not be merged** survive exactly the moment
someone is refactoring — which is the moment advice fails. And the upload was
changed to **byte-copy `cells.data()`** with no packing step, so the cell type is
now a build error rather than a silently misaligned 2 MB upload.

This is a **family**, and two more members landed in the same fix:

| member | silent failure it replaces |
|---|---|
| `static_assert(sizeof(glm::vec4) == 16)` | a vec4 reinterpretation that is wrong only on one layout |
| `kBytes` tied to `kN^3 * sizeof` | a buffer size that is a *number* instead of a *consequence* |
| upload byte-copies `cells.data()` | a packing step that can disagree with the producer |
| test keeps its **own** `indexOf` | the closed loop itself |

**Prefer a constraint the compiler enforces over a review step a future session
may not run.** A `static_assert` cannot be talked out of; a comment explaining a
convention can.

### Assertion count and independent-check count move independently

Vega's fix took the suite from **232 assertions to 153** — the count went *down*
— while **97 of the original assertions had been about the wrong region**, and
the ray walk went from **186 cells over 6 rays** (broken) to **89 over 3**
(fixed). So the count fell while the coverage of the thing under test improved.

> **A falling assertion count with rising independent coverage is not a weaker
> suite. Reading the count as strength cannot distinguish the two, which is the
> clearest possible demonstration that it is the wrong thing to read.**

Resulting build state: `vf_tests` 6/6 with **153** assertions, `test-world` 2/2.

### A model of an instrument is not the instrument

A Python model of the expression predicted float32 truncation would break **4**
cells (3, 5, 10, 11). The compiled C++ expression fails on **8** — 3, 5, 8, 10,
23, 44, 49, 54. Same quantity, twice the cells.

Note the source of the wrong figure: it came from the same author who had spent
the session establishing that models are not measurements. Nothing in the
presentation differs — the number simply arrives in the shape of one.

> **A model of the thing under test produces numbers that are indistinguishable
> from measurements by form.** Only provenance separates them, which means
> provenance has to be *stated*, not inferred from the notation.

The practical rule this generalises to: **the gate is the instrument.** Vega's
`static_assert` / `indexOf` gate is compiled and was **proven to fire** — he
reverted `lround` to truncation, watched it report all 8 cells **by name**,
restored, and **md5-verified** the restore. That is the positive firing test
[[concepts/focused-test-groups]]'s gate checks exist to require, and it is also
the direct answer to the stale-binary hazard below: an md5-verified restore is
what distinguishes "I put it back" from "the artifact is what I think it is".

### A build that failed can leave the previous binary running

Part of Vega's near-miss: a failed build silently ran a **stale binary**, so an
instrument reported from an artifact nobody was editing. The general form is
independent of irradiance — this is the documented "`ninja` says *no work to do*"
hazard biting in practice, and it means **a shader or source edit does not imply
the binary changed**.

**Two separate questions, both of which must be answered:** *which source was
written* and *which artifact ran*. On a dirty tree the answer to the second can
be "neither". Ask for the md5 of the binaries actually run alongside
`git rev-parse HEAD` and the dirty-path list — see
[[concepts/measurement-provenance]].

### The escape route, concretely

Derive at least one expectation from an **independent authority** — never from
the code under test:

- `VoxelField::sampleWorld`: `int((p.x + 0.5f * WORLD) / VOXEL)`
- `--probe`, which reads the live layered field rather than the bake
- the terrain's own `kHmMinMeters = -8.0f` (`heightmap.hpp`)

If you cannot name the external reference your assertions actually check
against, they are not asserting correctness — they are asserting that the
implementation agrees with itself.

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
