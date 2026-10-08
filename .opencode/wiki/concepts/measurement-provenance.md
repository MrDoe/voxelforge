---
title: "A number is not a gate until it carries its command, its metric definition and its tree state"
tags: [measurement, provenance, testing, visual-check, reproducibility, wiki-hygiene]
sourceRefs: [tests/visual_check.py, .opencode/wiki/concepts/sky-probe-is-a-camera-assertion.md, .opencode/wiki/concepts/overlay-silent-write-trap.md, .opencode/wiki/concepts/measurement-discipline.md]
lastReviewed: 2026-10-07
---

# Measurement provenance

A mean-luma / dark-% / blue-% figure is not a threshold until three things
travel with it:

1. **the exact command** (binary, `--cam`, resolution, mode, env),
2. **the metric definition** (which pixels, which threshold, which region),
3. **the tree state** it was measured on.

Drop any one and the number is real but not comparable, and the failure is
silent: nothing in the output says which setup produced it. This page exists
because all three were dropped in one exchange, in three different ways, and the
resulting confusion cost more than the renders did.

## The three failures, as actually observed

**Missing source (the dangerous one).** A sun-arm table in
[[concepts/sky-probe-is-a-camera-assertion]] carried the setup "640x360, hero
cam `1.0 2.0 1.5 → 5.3 1.0 11.3`, splat". The session that produced the numbers
could not quote the camera — its scripts were in `/tmp` and a reboot erased
them. The attribution had been written into the wiki **from memory**, with no
artifact, and read back as fact by a later session, which then *chose its own
camera by reading that very line*. So the second measurement believed it matched
the first, and the two pairs' absolute columns were compared for a day before
anyone noticed there was no evidence they shared a setup.

The compounding step is the lesson: **an unsourced attribution in a durable page
does not stay a small wrongness — it becomes an input to the next experiment.**

**Missing metric definition.** Two pairs both reported "dark %" and "top-eighth
blue %". Neither recorded what those meant. One used `(r+g+b)/3 < 30` for dark
and the top `H/8` rows with `b >= r` for blue; the other was never written down.
So `0.98 %` and `0.01 %` were not known to be comparable, and a confident
explanation for the gap — "different metric definitions" — was proposed and
briefly believed. It was wrong.

**Missing tree state.** Arms measured on different trees, hours and commits apart,
were placed in adjacent rows of one table with a "compare within a column, never
across them" warning that nobody could enforce, because nothing in the table
recorded *which* tree each row came from.

**The mirror image: an attributed measurement.** The most recent instance, and the
one easiest to miss because it reads like a citation rather than an omission. A
session wrote *"my settled night frame is dark% ~70 and blue% ~85"* — those
figures were another session's pair, and it had attached its own name to them
**while arguing against that very pair**. On request it confirmed the attribution
was wrong. So the number existed, was correctly computed, and was in no form
measured by the person who said it. An omission (no source) is at least visible;
an over-attribution is laundered by being repeated confidently, and it is
reinforced every time a later reader cites it. The question that catches it is
the inverse of the one in the previous section: not "what was the command?" but
**"did you measure that, or are you quoting it?"**

## What survives without provenance, and what does not

**Ratios survive; absolutes do not.** The night/day luma ratio came out 0.278
in one pair and 0.273 in the other — 2 % agreement — while the absolute means
disagreed by ~5 % and the dark-% column by two orders of magnitude. A ratio is
the one quantity a camera change or content drift cannot fake, because both
largely cancel between numerator and denominator. So a shared invariant is the
only thing worth quoting across setups; an absolute mean is not.

**A hypothesis is not a finding.** The overlay explanation for the gap (one pair
loaded the 24.6 MB runtime overlay, the other did not) was plausible, cheap to
test, and written down before being checked. It was **refuted** when all three
sets turned out to be overlay-suppressed. It had been sitting in a wiki page as
the leading explanation, where the next reader would have inherited it.

## The rules that follow

- **Ask for the command, not the value.** "What was the command?" is free and
  answers all three questions at once. Re-deriving a setup by guessing costs GPU
  time and may be impossible.
- **Mark provenance at write time, not at cite time.** A wiki page is a durable
  store: anything written into it needs a *source* at the moment of writing —
  a log, a commit, a command. If there isn't one, write "unsourced" in the same
  breath. Retro-fitting the label after someone relies on the line is how the
  camera attribution survived as long as it did.
- **Record the metric definition next to the number**, every time. It is two
  lines and it is the difference between a comparison and a coincidence.
- **Unsourced is not disproved.** Label the *status* honestly and separately from
  the claim: an unverifiable setup may well be correct (the reference camera was
  plausible), and writing "retracted" would have been an overclaim in the other
  direction. See the `⚠ UNSOURCED` box on
  [[concepts/sky-probe-is-a-camera-assertion]].
- **`/tmp` is not a store.** A measurement whose only record is a script in
  `/tmp` is unreproducible by construction — two reboots during this project
  erased exactly that. Preservation snapshots and measurement scripts belong
  outside `/tmp`; see [[concepts/overlay-silent-write-trap]].
- **An experiment whose failure mode is data loss is not a pending
  experiment — it is an excluded one.** The one candidate explanation left for
  the day/night discrepancy was an overlay-loaded vs overlay-suppressed A/B. It
  was the only step in the exchange that could destroy data (a headless run with
  neither `VF_NO_OVERLAY` nor `VF_OVERLAY_PATH` both loads *and rewrites*
  `assets/runtime_edits.vxw`, and the writer serialises only `Chunk::edited` —
  measured once shrinking a session from 3.26 MB to 619 KB, silently). Once the
  hypothesis was refuted by asking rather than rendering, the arm was struck off
  permanently rather than parked. Weigh an arm's **expected information against
  its blast radius**: when the only way to learn something is a command that can
  destroy a peer session's work, the information is not worth the risk, and
  "we could try it carefully with a copy" is a cost, not a free workaround.

## Before quoting any figure from another session

Can a reader run the quoted command and land on the number? If not, it is
**provisional** and must be labelled so, in the same place the number lives.
This is the same discipline as the daylight-calibration table in
[[concepts/sky-probe-is-a-camera-assertion]] (a content-derived constant read as
a universal one) and as the off-state control rule in
[[concepts/measurement-discipline]] — an assertion is only worth what its setup
is worth, and "I set the flag" is not the same claim as "the feature is off".

Cross-links: [[concepts/sky-probe-is-a-camera-assertion]] (the instance that
produced this page), [[concepts/overlay-silent-write-trap]] (the silent variable
that was *almost* the culprit),
[[concepts/splat-edge-fade-measurement]] (another measurement leaning on an
off-state control), [[concepts/sun-direction-pipeline]] (the day/night lane the
arms belong to), [[concepts/focused-test-groups]] (which group runs which
script).