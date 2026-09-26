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

## The five instances (each one nearly filed as a finding)

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
