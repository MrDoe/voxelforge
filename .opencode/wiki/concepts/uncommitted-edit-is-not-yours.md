---
title: "A leading ` M` is not yours to restore"
tags: [git, attribution, assets, data-loss, world.json, hazard]
sourceRefs:
  - assets/world.json
  - AGENTS.md
lastReviewed: 2026-10-08
---

# A leading ` M` is not yours to restore

## The rule

> A leading ` M` (uncommitted modification) in `git status` means the file is
> **not yours to restore** — whatever you think you know about how it got
> there.

The overlay file is gitignored so its damage is invisible in `git status`
([[concepts/overlay-silent-write-trap]]). This page is the complementary
hazard: a **tracked** file shows as ` M`, and it is equally not yours to
overwrite, revert, `git checkout`, or `git stash`.

## What happened (2026-10-06) — final

`assets/world.json` was found already ` M` by the shading session, which was
about to inject a temporary `"lights"` block for a render test. The shading
session **did not author that edit** — it was pre-existing, from a session
before it (the file's keys are only `version` / `layers` / `textures` — no
`sun`, no `lights` — so the edit is inside the layer list or the textures
table).

It then ran `git checkout --` on the file to get a clean baseline, which
**discarded the unknown session's edit**. It reconstructed the bytes from a
scratch copy and restored them.

**Final state (re-verified):** md5 `dac9f0592bd8d408879b269676e2109e`, valid
JSON, keys `version`/`layers`/`textures`, still ` M` — byte-identical to what
was found before it was touched.

> **Settled in state, unresolved in ownership.** Pre-existing uncommitted edit,
> unknown session, destroyed by a stray `git checkout --` and byte-exact
> restored via md5-forensic reconstruction from a scratch copy.
> **Ownership is unknown and is not going to be resolved** — the session that
> found it never saw the author. Do not upgrade this to an attribution, and do
> not read it as still-in-progress.

## The load-bearing lesson is the scratch copy, not the hashing

The restore **would not have worked** without the scratch copy — a
`cp -r assets/. /tmp/opencode/scene/` made *minutes earlier for an unrelated
reason*. The md5 comparison is how the recovery was *verified*, not how it was
*achieved*.

So the rule that matters is **back up before touching a shared file**, not the
forensic trick. A backup taken for an unrelated reason was the only thing
between a stray checkout and permanent loss of another session's work.

## Why the attribution matters more than the incident

The content was recovered exactly, so the cost here was near zero. The
dangerous outcome is the *belief*: if a later session hears "the world.json
edit is George's", it may safely clobber it, and this time there may be no
scratch copy to recover from. Provenance you inferred from "I was here when it
appeared" is not ownership.

## Recurrence in `src/` (2026-10-08) — and it was not recoverable

A branch checkout in a dirty tree **overwrote uncommitted `FalloffCurve`
definitions in `src/voxel/editable_world.cpp`**, breaking `vf_core`. The
reporting session reverted the file to `HEAD` and released its claim rather than
re-applying over the top. Unlike the `assets/` incident, **nothing was
recoverable**: no `git stash`, no `.orig`/`.rej`/swap files, and `HEAD` never
contained the code.

What survived was enough to rebuild, and that is the difference:

| | `assets/world.json` (2026-10-06) | `editable_world.cpp` (2026-10-08) |
|---|---|---|
| recoverable? | **yes** — byte-exact from a scratch copy | **no** — only re-implementable |
| what remained | the exact bytes | a declared contract in the surviving `.hpp` (signatures, per-function intent in comments) and **expected values pinned in `tests/test_editable.cpp`** (`Sphere` 0.87, `Root` 0.71, `Smooth` 0.50, `Sharp` 0.25 at q=0.5, plus q=0/1) |

> **The prevention below was written for `assets/` and only ever applied there.**
> A scratch copy turns an unrecoverable loss into an inconvenience, so the
> cheapest fix is not a git habit — it is **copying the tree you are about to
> switch branches on, whichever directory it is**.

Two things that made this survivable rather than total, worth keeping as the
positive case: the implementation was **inline in the header**, so
`git show HEAD:….cpp | grep -c FalloffCurve` returning `0` proved the curve
math was never lost; and the tests pinned numeric acceptance values. A missing
implementation behind an intact signature plus a test suite is a spec, not a loss.

### Second recurrence: `TextureBinding::emissive` (2026-10-08) — same mode, re-implemented

Same day, same mode: a port checkout in a dirty tree took an uncommitted
extension — `TextureBinding::emissive` (`bool`, default `false`) +
`emissiveScale` (`float`, default `1.0`) in `worldfile.hpp`, with the
textures-table parse (`j.boolean`/`j.num` on keys `"emissive"`/`"emissiveScale"`)
and emit in `worldfile.cpp`. Downstream (`texture_atlas.cpp`,
`panel_textures.cpp`) needed the field; the header no longer had it; `voxelforge`
failed to build. Attribution stays **prime-suspect-unconfirmed** (owner
unreachable) — filed as the mechanism, not the culprit.

Recovery, confirmed by two sessions: Fledge re-implemented the extension **from
the usage sites** under coordinator authorization (re-implemented, not
re-applied — the original bytes are gone with their owner). The re-implementation
restores the documented invariant rather than inventing one: the writer emits
the pair **only when set**, so legacy manifests stay byte-identical.

| | `editable_world.cpp` (instance one) | `worldfile.hpp/.cpp` (instance two) |
|---|---|---|
| recoverable? | no — re-implementable from header + pinned tests | no — re-implementable from usage sites |
| what remained | signatures, comments, `test_editable` values | call sites needing the field, byte-identity invariant |
| verification | `test_editable` green (15/15, 3037/3037) | scratch round-trip PASS + byte-identity PASS; **durable committed gate still open** (`test_worldfile.cpp` is Victor's claim) |

The pattern is now two instances in one day: **a checkout in a dirty tree is
the most destructive ordinary operation in this repo**, and it selects exactly
the state git cannot restore. The prevention below is unchanged — it just
stops being advice about `assets/` and becomes a rule about *any* tree.

### A checkout in a dirty tree is destructive to unversioned state

This belongs on the same list as `ninja -C build clean`
([[concepts/authored-assets-are-build-outputs]]) and `rm` with a glob near the
overlay ([[concepts/overlay-silent-write-trap]]): an operation that **succeeds,
reports success, and destroys state git does not track.** `git stash push -u`
before any branch switch is one line and closes it.

## Prevention

1. `git status` first, every session, before touching anything under
   `assets/`.
2. A file that is ` M` on arrival belongs to someone else. Assume so.
3. **Copy the tree aside before you touch it** — `cp -r assets/. /tmp/...` for
   an unrelated reason is what saved this one. Do this *first*; it costs
   nothing and is the only thing that makes a stray `checkout` recoverable.
   **Scope it to whatever you are about to touch, not just `assets/`** — the
   2026-10-08 `src/` recurrence had no scratch copy and was not recoverable.
4. **Stash before any branch switch in a dirty tree** (`git stash push -u`).
   A checkout reports success while discarding untracked-to-git work.
5. If you need a clean baseline, restore from the copy, never from memory.
5. `assets/` is versioned but its manifests are rewritten by tests and by
   in-app actions (see [[concepts/authored-assets-are-build-outputs]]), so
   ` M` there is common and usually benign — which is exactly why it must be
   treated as someone else's.