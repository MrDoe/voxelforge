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
