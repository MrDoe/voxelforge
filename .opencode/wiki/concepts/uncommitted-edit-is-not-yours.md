---
title: "A leading ` M` is not yours to restore"
tags: [git, attribution, assets, data-loss, world.json, hazard]
sourceRefs:
  - assets/world.json
  - AGENTS.md
lastReviewed: 2026-10-06
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

## Prevention

1. `git status` first, every session, before touching anything under
   `assets/`.
2. A file that is ` M` on arrival belongs to someone else. Assume so.
3. **Copy the tree aside before you touch it** — `cp -r assets/. /tmp/...` for
   an unrelated reason is what saved this one. Do this *first*; it costs
   nothing and is the only thing that makes a stray `checkout` recoverable.
4. If you need a clean baseline, restore from the copy, never from memory.
5. `assets/` is versioned but its manifests are rewritten by tests and by
   in-app actions (see [[concepts/authored-assets-are-build-outputs]]), so
   ` M` there is common and usually benign — which is exactly why it must be
   treated as someone else's.